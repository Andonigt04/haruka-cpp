#include "core/weather_system.h"

#include <glm/gtc/constants.hpp>
#include <cmath>

namespace Haruka {

namespace {

// Hash entero → [0,1). Determinista y barato; los frentes se siembran con esto, no con rand().
inline float h01(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return (float)(x & 0x00FFFFFFu) / (float)0x01000000u;
}
inline float hashf(uint32_t seed, int i, int k) {
    return h01(seed * 747796405u + (uint32_t)i * 2891336453u + (uint32_t)k * 2654435761u);
}

// Punto uniforme en la esfera a partir de dos uniformes [0,1). Uniforme DE VERDAD (z uniforme, no
// el ángulo): sembrar con (lat,lon) uniformes amontonaría todos los frentes en los polos.
inline glm::dvec3 spherePoint(float u1, float u2) {
    const double z   = 2.0 * (double)u1 - 1.0;
    const double r   = std::sqrt(std::max(0.0, 1.0 - z * z));
    const double phi = 2.0 * glm::pi<double>() * (double)u2;
    return glm::dvec3(r * std::cos(phi), r * std::sin(phi), z);
}

// Rodrigues: gira `v` un ángulo `ang` alrededor del eje unitario `axis`.
inline glm::dvec3 rotateAround(const glm::dvec3& v, const glm::dvec3& axis, double ang) {
    const double c = std::cos(ang), s = std::sin(ang);
    return v * c + glm::cross(axis, v) * s + axis * glm::dot(axis, v) * (1.0 - c);
}

// El NORTE local del planeta es +Y: es el mismo eje con el que `PlanetFields::computeClimate`
// calcula la latitud para la temperatura. Si aquí se usara otro, los vientos zonales (alisios,
// poniente de latitudes medias) no casarían con las bandas de temperatura y se vería raro.
constexpr glm::dvec3 kNorth{0.0, 1.0, 0.0};

} // namespace

void WeatherSystem::configure(uint32_t seed) {
    if (m_configured && m_seed == seed) return;   // idempotente: reconfigurar movería las tormentas
    m_seed = seed;

    for (int i = 0; i < kFronts; ++i) {
        Front& f = m_fronts[i];

        // Eje de giro y punto de partida, independientes → el frente describe un círculo máximo
        // inclinado cualquiera. Con el eje SIEMPRE en +Y todos los frentes irían en paralelo por
        // latitudes fijas y el planeta tendría "carriles" de tormenta.
        f.axis  = glm::normalize(spherePoint(hashf(seed, i, 0), hashf(seed, i, 1)));
        glm::dvec3 s = spherePoint(hashf(seed, i, 2), hashf(seed, i, 3));
        s -= f.axis * glm::dot(f.axis, s);                       // al plano perpendicular al eje
        const double sl = glm::length(s);
        f.start = (sl > 1e-9) ? s / sl : glm::normalize(glm::cross(f.axis, glm::dvec3(1, 0, 0)));

        // Velocidad: unos van más rápido que otros y ~1/3 van al revés → los frentes se alcanzan,
        // se cruzan y se separan. Con todos a la misma velocidad el patrón sería rígido.
        const double rate = 0.55 + 0.9 * (double)hashf(seed, i, 4);
        const double sign = (hashf(seed, i, 5) < 0.35f) ? -1.0 : 1.0;
        f.omega = sign * rate * 2.0 * glm::pi<double>() / kFrontRevolutionS;

        // Tamaño y fuerza. El radio angular manda en la ESCALA: 0.25 rad sobre la Tierra son ~1600 km
        // de radio = una borrasca de verdad; 0.72 tapa medio hemisferio (un sistema de gran escala).
        f.radius   = 0.25 + 0.47 * (double)hashf(seed, i, 6);
        f.strength = 0.55f + 0.45f * hashf(seed, i, 7);
    }
    m_configured = true;
}

glm::dvec3 WeatherSystem::frontCenter(int i) const {
    const Front& f = m_fronts[i];
    return rotateAround(f.start, f.axis, f.omega * m_time * m_timeScale);
}

float WeatherSystem::cloudCoverAt(const glm::dvec3& dir, float humidity) const {
    if (!m_configured) return 0.0f;
    const glm::dvec3 d = glm::normalize(dir);

    // Cobertura = lo que aportan los frentes que tapan el punto. Se suma en vez de tomar el máximo
    // para que dos frentes solapados den cielo REALMENTE cerrado (el clamp final lo acota).
    float cover = 0.0f;
    for (int i = 0; i < kFronts; ++i) {
        const Front& f = m_fronts[i];
        const double cosang = glm::clamp(glm::dot(d, frontCenter(i)), -1.0, 1.0);
        const double ang    = std::acos(cosang);
        if (ang >= f.radius) continue;
        // Perfil suave del casquete: 1 en el núcleo → 0 en el borde. `t·t·(3−2t)` = smoothstep.
        const double t = 1.0 - ang / f.radius;
        cover += f.strength * (float)(t * t * (3.0 - 2.0 * t));
    }

    // La humedad del CAMPO modula: el mismo frente descarga sobre la selva y cruza el desierto casi
    // vacío. Y un fondo de nube proporcional a la humedad (la selva rara vez está del todo despejada).
    const float H = glm::clamp(humidity, 0.0f, 1.0f);
    return glm::clamp(cover * (0.45f + 0.75f * H) + 0.12f * H, 0.0f, 1.0f);
}

WeatherSample WeatherSystem::sampleAt(const glm::dvec3& dir, float tempC, float humidity) const {
    WeatherSample w;
    w.tempC    = tempC;
    w.humidity = glm::clamp(humidity, 0.0f, 1.0f);
    if (!m_configured) return w;

    const glm::dvec3 d = glm::normalize(dir);
    w.cloudCover = cloudCoverAt(d, humidity);

    // ── LA PRECIPITACIÓN SALE DE LA COBERTURA ───────────────────────────────────────────────────
    // Esta línea ES el sistema: no hay ningún otro camino por el que `precip` pueda subir. Sin nube
    // encima (cover < kPrecipCover) el resultado es exactamente 0, y el aire seco no descarga
    // aunque el cielo esté cerrado (una capa alta sobre el desierto tapa el sol y no moja).
    const float heavy = glm::smoothstep(kPrecipCover, 0.94f, w.cloudCover);
    w.precip = glm::clamp(heavy * (0.30f + 0.70f * w.humidity), 0.0f, 1.0f);
    if (w.precip > 0.01f) w.type = (tempC <= kSnowTempC) ? Precip::Snow : Precip::Rain;
    else                  { w.precip = 0.0f; w.type = Precip::None; }

    // Base de la nube ≈ nivel de condensación: cuanto más seco el aire, más alto condensa. Es el
    // TECHO desde el que cae la precipitación (la fase B la dibuja entre esta cota y el suelo), así
    // que no es decorativo: si está mal, la lluvia nace dentro de la nube o muy por debajo.
    w.cloudBaseM = glm::clamp(320.0f + (1.0f - w.humidity) * 2100.0f
                              - glm::smoothstep(25.0f, -5.0f, tempC) * 260.0f,
                              200.0f, 2600.0f);

    // ── VIENTO ──────────────────────────────────────────────────────────────────────────────────
    // Marco tangente local (este/norte) para poder hablar de vientos zonales como en la Tierra.
    glm::dvec3 east = glm::cross(kNorth, d);
    const double el = glm::length(east);
    east = (el > 1e-6) ? east / el : glm::dvec3(1, 0, 0);       // en el polo el "este" degenera
    const glm::dvec3 north = glm::cross(d, east);

    // (1) Circulación de fondo por bandas de latitud: alisios cerca del ecuador, poniente en
    //     latitudes medias. `sin(2.6·lat)` cambia de signo hacia los 35° → las dos bandas.
    const double lat   = std::asin(glm::clamp(glm::dot(d, kNorth), -1.0, 1.0));
    const double zonal = -5.5 * std::sin(2.6 * lat) * std::cos(lat);
    glm::dvec3 wind = east * zonal;

    // (2) Cada frente arrastra el aire EN SU DIRECCIÓN DE AVANCE, con el peso de cuánto tapa. Así el
    //     viento gira cuando la tormenta pasa por encima, que es lo que se nota al jugar: primero
    //     cambia el viento, luego llega la nube. La velocidad del frente en el punto es ω × r.
    for (int i = 0; i < kFronts; ++i) {
        const Front& f = m_fronts[i];
        const glm::dvec3 c = frontCenter(i);
        const double ang = std::acos(glm::clamp(glm::dot(d, c), -1.0, 1.0));
        if (ang >= f.radius) continue;
        const double t = 1.0 - ang / f.radius;
        const double wgt = f.strength * t * t * (3.0 - 2.0 * t);
        glm::dvec3 adv = glm::cross(f.axis, d);                  // dirección de avance en ESTE punto
        const double al = glm::length(adv);
        if (al < 1e-9) continue;
        wind += (adv / al) * (f.omega * 6371000.0 * 0.35) * wgt; // ω·R escalado a m/s plausibles
    }

    // (3) Racha: variación lenta y determinista (nada de rand()). ±25 % sobre el vector.
    const float gust = 0.75f + 0.5f * h01((uint32_t)(m_time * 0.35) * 2654435761u + m_seed);
    wind *= (double)gust;

    // La lluvia arrecia con el viento (la borrasca sopla y descarga a la vez): +30 % como techo.
    const double speed = glm::length(wind);
    w.precip = glm::clamp(w.precip * (float)(1.0 + 0.02 * glm::min(speed, 15.0)), 0.0f, 1.0f);
    w.wind   = glm::vec3(wind);
    return w;
}

} // namespace Haruka
