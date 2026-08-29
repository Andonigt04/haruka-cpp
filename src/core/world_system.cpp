#include "world_system.h"
#include "game/planetary_system.h"
#include "core/scene/scene_manager.h"
#include "tools/object_types.h"
#include <cmath>

namespace Haruka {

// Duración de un día completo (s). Showcase: ~4 min para que se note el ciclo sin
// marear. (Futuro: exponer como ajuste.)
static constexpr double kDayLengthSeconds = 240.0;
// Periodo orbital de la Luna (s). Más lento que el día → se mueve por el cielo.
static constexpr double kMoonPeriodSeconds = 1800.0; // 30 min/órbita: deriva visible pero suave

    WorldSystem::WorldSystem() : m_worldOrigin(0.0, 0.0, 0.0) {}

    WorldSystem::~WorldSystem() = default;

    void WorldSystem::init() {
        m_planetarySystem = std::make_unique<PlanetarySystem>();
        m_planetarySystem->init();
        
        // Aquí podrías cargar tu base de datos de planetas inicial
    }

    void WorldSystem::update(double dt, const glm::dvec3& cameraPos) {
        // 1. GESTIÓN DE PRECISIÓN (Floating Origin)
        // Si la cámara se aleja más de 5000km del origen actual, reseteamos el origen.
        if (glm::length(cameraPos) > 5000.0) {
            shiftOrigin(cameraPos);
        }

        // 2. ACTUALIZAR SUBSISTEMAS
        // Pasamos la cámara y una matriz de vista-proyección (calculada con toLocal)
        m_planetarySystem->update(dt, cameraPos);
    }

    void WorldSystem::advanceCelestial(double dt) {
        // La mecánica celeste corre cada frame desde el render (no desde update(),
        // que no se llama) y usa el planeta REAL fijado vía setActivePlanet().
        if (!m_hasActivePlanet) return;
        updateDayNight(dt); // órbita del Sol → día/noche + luz/sombras
        // La Luna YA puede orbitar: sus chunks son planet-local y el renderer les suma
        // el centro ACTUAL del planeta cada frame → mover el cuerpo no rompe el
        // streaming (no se regeneran chunks, solo se trasladan).
        updateMoon(dt);
        updateTide();       // marea (alineación Sol–Luna)
        m_atmoTime += dt;   // deriva/ráfagas del viento
    }

    float WorldSystem::getSunElevation(const glm::dvec3& observer) const {
        if (!m_hasActivePlanet) return 1.0f;
        glm::dvec3 up = observer - m_planetCenter; double ul = glm::length(up);
        if (ul < 1e-9) return 1.0f;
        up /= ul;
        glm::vec3 sunDir = getDominantLightDirection(observer); // hacia el Sol
        return (float)glm::dot(glm::dvec3(sunDir), up);
    }

    glm::vec3 WorldSystem::getSkyColor(const glm::dvec3& observer) const {
        if (!m_hasActivePlanet) return glm::vec3(0.005f);
        const glm::dvec3 pc = m_planetCenter; const double pr = m_planetRadius;
        glm::dvec3 up = observer - pc; double ul = glm::length(up);
        if (ul < 1e-9) return glm::vec3(0.005f);
        up /= ul;
        float el = (float)glm::dot(glm::dvec3(getDominantLightDirection(observer)), up);

        // Cielo por elevación solar: noche → día → resplandor cálido en el horizonte.
        float day = glm::smoothstep(-0.10f, 0.25f, el);
        glm::vec3 night = glm::vec3(0.015f, 0.02f, 0.05f);
        glm::vec3 dayC  = glm::vec3(0.40f, 0.60f, 0.92f);
        glm::vec3 sky   = glm::mix(night, dayC, day);
        float sunset = std::exp(-(el * el) / 0.02f) * glm::smoothstep(-0.25f, 0.05f, el);
        sky = glm::mix(sky, glm::vec3(0.85f, 0.42f, 0.22f), sunset * 0.7f);

        // Fade a NEGRO espacial al subir (la atmósfera tiene grosor finito).
        double alt  = ul - pr;
        float  atmo = 1.0f - glm::smoothstep(0.0f, (float)(pr * 0.02), (float)alt);
        return glm::mix(glm::vec3(0.004f, 0.004f, 0.008f), sky, atmo);
    }

    glm::vec3 WorldSystem::getWind(const glm::dvec3& worldPos) const {
        if (!m_hasActivePlanet) return glm::vec3(0.0f);
        const glm::dvec3 pc = m_planetCenter; const double pr = m_planetRadius;
        glm::dvec3 up = worldPos - pc; double ul = glm::length(up);
        if (ul < 1e-9) return glm::vec3(0.0f);
        up /= ul;
        // Sin viento fuera de la atmósfera (espacio).
        float atmo = 1.0f - glm::smoothstep(0.0f, (float)(pr * 0.02), (float)(ul - pr));
        if (atmo <= 0.001f) return glm::vec3(0.0f);

        // Dirección tangente que gira lento + ráfagas (suma de senos).
        glm::dvec3 ref = (std::abs(up.y) < 0.99) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
        glm::dvec3 T = glm::normalize(glm::cross(ref, up));
        glm::dvec3 B = glm::cross(up, T);
        double a = m_atmoTime * 0.03;
        glm::dvec3 dir = T * std::cos(a) + B * std::sin(a);
        float gust = 4.0f + 2.5f * (float)std::sin(m_atmoTime * 0.7)
                          + 1.5f * (float)std::sin(m_atmoTime * 0.23 + 1.0);
        return glm::vec3(dir) * (gust * atmo); // m/s
    }

    void WorldSystem::updateTide() {
        if (!m_hasActivePlanet) { m_tideFactor = 1.0f; return; }
        const glm::dvec3 pc = m_planetCenter;

        // Dirección al Sol (primera estrella) y a la Luna (objeto de escena).
        const CelestialBody* sun = nullptr;
        for (const auto& b : m_bodies) { if (b.type == ObjectType::STAR) { sun = &b; break; } }
        std::shared_ptr<SceneObject> moon = m_scene ? m_scene->getObject("Moon") : nullptr;
        if (!sun || !moon) { m_tideFactor = 1.0f; return; }

        glm::dvec3 sunD  = sun->worldPos - pc;
        glm::dvec3 moonD = glm::dvec3(moon->position) - pc;
        double sl = glm::length(sunD), ml = glm::length(moonD);
        if (sl < 1e-9 || ml < 1e-9) { m_tideFactor = 1.0f; return; }

        // Alineación (0..1): 1 cuando Sol y Luna están en la misma línea (viva),
        // 0 en cuadratura (muerta). |cos| porque luna nueva Y llena dan marea viva.
        double align = std::fabs(glm::dot(sunD / sl, moonD / ml));
        m_tideFactor = (float)glm::mix(0.55, 1.35, align); // amplitud del oleaje
    }

    float WorldSystem::getTideHeight(const glm::dvec3& observer) const {
        if (!m_hasActivePlanet) return 0.0f;
        glm::dvec3 up = observer - m_planetCenter; double ul = glm::length(up);
        if (ul < 1e-9) return 0.0f; up /= ul;

        // Bulto de marea ∝ (3·cos²θ − 1)/2, θ = ángulo cuerpo↔up local. +1 en cénit/nadir
        // (pleamar), −0.5 en el horizonte (bajamar). Luna domina; Sol ~0.46×.
        auto bulge = [&](const glm::dvec3& bodyWorld, double weight) -> double {
            glm::dvec3 d = bodyWorld - m_planetCenter; double l = glm::length(d);
            if (l < 1e-9) return 0.0;
            double c = glm::dot(d / l, up);
            return weight * (3.0 * c * c - 1.0) * 0.5;
        };
        double h = 0.0;
        std::shared_ptr<SceneObject> moon = m_scene ? m_scene->getObject("Moon") : nullptr;
        if (moon) h += bulge(glm::dvec3(moon->position), 1.0);
        const CelestialBody* sun = nullptr;
        for (const auto& b : m_bodies) { if (b.type == ObjectType::STAR) { sun = &b; break; } }
        if (sun) h += bulge(sun->worldPos, 0.46);

        const double kTideAmplitudeM = 0.7; // amplitud suave para que no rompa la costa
        return (float)(h * kTideAmplitudeM);
    }

    void WorldSystem::updateMoon(double dt) {
        if (!m_scene || !m_hasActivePlanet) return;
        auto moon = m_scene->getObject("Moon");
        if (!moon) return;
        const glm::dvec3 pc = m_planetCenter;

        if (!m_moonInit) { m_moonOffset0 = glm::dvec3(moon->position) - pc; m_moonInit = true; }
        m_moonAngle += dt * (2.0 * M_PI / kMoonPeriodSeconds);

        // Órbita circular INCLINADA (no plana): el plano orbital se inclina un ángulo `incl`
        // respecto al ecuador y se gira por el nodo ascendente `node` → la Luna cruza el cielo en
        // diagonal, como la Luna real (~5° real; aquí ~25° para que se note). Radio = distancia inicial.
        const double R    = glm::length(m_moonOffset0);
        const double incl = 0.44;  // inclinación del plano orbital (rad, ~25°)
        const double node = 0.7;   // longitud del nodo ascendente (rad) → orienta el plano
        // Normal del plano orbital (inclinada desde +Y) y base (u,v) perpendicular a ella.
        const glm::dvec3 n(std::sin(incl) * std::cos(node), std::cos(incl), std::sin(incl) * std::sin(node));
        glm::dvec3 u = glm::normalize(glm::cross(n, glm::dvec3(0.0, 1.0, 0.001)));
        glm::dvec3 v = glm::cross(n, u);
        moon->position = pc + (u * std::cos(m_moonAngle) + v * std::sin(m_moonAngle)) * R;
    }

    void WorldSystem::updateDayNight(double dt) {
        if (!m_hasActivePlanet) return;
        const glm::dvec3 pc = m_planetCenter;

        // Localiza el Sol (primera estrella). Guarda su offset inicial al planeta.
        CelestialBody* sun = nullptr;
        for (auto& b : m_bodies) { if (b.type == ObjectType::STAR) { sun = &b; break; } }
        if (!sun) return;
        if (!m_celestialInit) {
            // Offset inicial desde el OBJETO DE ESCENA (absoluto, nunca desplazado por
            // el floating-origin), no desde el body (que sí se desplaza) → frame correcto.
            glm::dvec3 sunAbs = sun->worldPos;
            if (m_scene) if (auto so = m_scene->getObject(sun->name)) sunAbs = glm::dvec3(so->position);
            m_sunOffset0 = sunAbs - pc;
            m_celestialInit = true;
        }

        m_dayAngle += dt * (2.0 * M_PI / kDayLengthSeconds);

        // ⚠️ `HARUKA_DAY_ANGLE=<radianes>` CONGELA la hora del dia. No es para jugar: es para poder
        // COMPARAR dos capturas. El angulo se acumula con el dt, asi que depende del tiempo de carga
        // y del ritmo de frames — dos ejecuciones del mismo binario cogen el Sol en sitios distintos y
        // la imagen cambia de brillo. Midiendo asi llegue a atribuir a Vulkan un frame "1,64x mas
        // claro" que en realidad estaba dentro de esa variacion: el mismo OpenGL daba 29,5 y 50,1 de
        // media en dos arranques. Sin fijar la hora, cualquier careo de color entre backends miente.
        { static const double s_fixed = [] {
              const char* e = std::getenv("HARUKA_DAY_ANGLE");
              return e ? std::atof(e) : -1.0; }();
          if (s_fixed >= 0.0) m_dayAngle = s_fixed; }

        // Rotación del Sol alrededor del EJE DEL PLANETA, que va INCLINADO ~23.5° (como el eje
        // real de la Tierra) en vez del eje Y recto → el terminador día/noche cruza en DIAGONAL y
        // la altura del Sol cambia con la latitud (sensación de estaciones/eje inclinado). Rodrigues.
        const double tilt = 0.41; // ~23.5° (inclinación axial)
        const glm::dvec3 axis = glm::normalize(glm::dvec3(std::sin(tilt), std::cos(tilt), 0.0));
        const glm::dvec3 o = m_sunOffset0;
        const double c = std::cos(m_dayAngle), s = std::sin(m_dayAngle);
        const glm::dvec3 rot = o * c + glm::cross(axis, o) * s + axis * (glm::dot(axis, o) * (1.0 - c));
        sun->worldPos = pc + rot;

        // Mueve también el Sol VISIBLE (objeto de escena) para que cruce el cielo.
        if (m_scene) {
            if (auto so = m_scene->getObject(sun->name)) so->position = sun->worldPos;
        }
    }

    void WorldSystem::shiftOrigin(const glm::dvec3& newOrigin) {
        // Calculamos cuánto nos hemos movido
        glm::dvec3 offset = newOrigin - m_worldOrigin;
        
        // Actualizamos el origen global
        m_worldOrigin = newOrigin;

        // Movemos todos los objetos del universo para compensar
        for (auto& body : m_bodies) {
            body.worldPos -= offset;
        }
    }

    void WorldSystem::syncFromScene(const SceneManager& scene) {
        // Guardamos la escena para poder mover el Sol/Luna VISIBLES en updateDayNight.
        m_scene = const_cast<SceneManager*>(&scene);
        m_bodies.clear();
        for (const auto& objPtr : scene.getAllObjects()) {
            if (!objPtr) continue;
            // Solo los cuerpos que EMITEN luz cuentan como fuente (sol). Un
            // CelestialBody no emisor (la Luna) NO entra aquí → no actúa como 2º sol.
            if (!objPtr->flags.castLight) continue;

            CelestialBody body;
            body.name     = objPtr->name;
            body.type     = ObjectType::STAR;
            body.worldPos = glm::dvec3(objPtr->position);
            body.radius   = std::max({(float)objPtr->scale.x, (float)objPtr->scale.y, (float)objPtr->scale.z});
            body.mass     = 1.989e30; // default solar mass
            body.color    = glm::length(glm::vec3(objPtr->color)) > 0.001f
                            ? glm::vec3(objPtr->color)
                            : glm::vec3(1.0f, 0.95f, 0.85f);
            m_bodies.push_back(body);
        }
    }

    glm::vec3 WorldSystem::getDominantLightDirection(const glm::dvec3& observerPos) const {
        glm::dvec3 sum(0.0);
        bool any = false;
        for (const auto& body : m_bodies) {
            if (body.type != ObjectType::STAR) continue;
            glm::dvec3 delta = body.worldPos - observerPos;
            double dist2 = glm::dot(delta, delta);
            if (dist2 < 1e-12) continue;
            sum += delta / dist2; // direction weighted by 1/dist²
            any = true;
        }
        // OJO: a escala astronómica |sum| ~ 1/dist es DIMINUTO (Sol a 1.5e11 → ~7e-12), así que
        // un umbral fijo (antes 1e-9) lo confundía con "no hay estrella" → devolvía el fallback
        // (0,1,0) → toda la iluminación/cielo apuntaba "arriba" (y el disco del cielo aparecía
        // separado de la corona = "dos soles"). Usamos un FLAG: si hubo estrella, normalizamos.
        if (!any || glm::dot(sum, sum) <= 0.0)
            return glm::vec3(0.0f, 1.0f, 0.0f);
        return glm::vec3(glm::normalize(sum));
    }

    bool WorldSystem::getMoonLight(const glm::dvec3& observer, glm::vec3& outDir, float& outIntensity) const {
        std::shared_ptr<SceneObject> moon = m_scene ? m_scene->getObject("Moon") : nullptr;
        if (!moon) return false;
        const CelestialBody* sun = nullptr;
        for (const auto& b : m_bodies) { if (b.type == ObjectType::STAR) { sun = &b; break; } }

        glm::dvec3 moonPos = glm::dvec3(moon->position);
        glm::dvec3 toMoon  = moonPos - observer;
        double ml = glm::length(toMoon);
        if (ml < 1e-6) return false;
        outDir = glm::vec3(toMoon / ml);            // dirección HACIA la Luna

        // Fase: fracción iluminada de la Luna vista desde el observador. Luna llena
        // (Sol detrás del observador) ≈ 1; nueva ≈ 0.
        float phase = 0.5f;
        if (sun) {
            glm::dvec3 moonToSun = glm::normalize(sun->worldPos - moonPos);
            glm::dvec3 moonToObs = glm::normalize(observer - moonPos);
            phase = (float)((glm::dot(moonToSun, moonToObs) + 1.0) * 0.5);
        }
        // Brillo base tenue (la luna ilumina ~1/400000 del sol; aquí, estilizado para
        // que la noche no sea negra pero siga siendo noche).
        outIntensity = phase * 0.22f;
        return true;
    }

    glm::vec3 WorldSystem::getDominantLightColor(const glm::dvec3& observerPos) const {
        glm::dvec3 colorSum(0.0);
        double weightSum = 0.0;
        for (const auto& body : m_bodies) {
            if (body.type != ObjectType::STAR) continue;
            glm::dvec3 delta = body.worldPos - observerPos;
            double dist2 = glm::dot(delta, delta);
            if (dist2 < 1e-12) continue;
            double w = 1.0 / dist2;
            colorSum   += glm::dvec3(body.color) * w;
            weightSum  += w;
        }
        if (weightSum <= 0.0)   // a escala astronómica w es DIMINUTO; el cociente da el color igual
            return glm::vec3(1.0f);
        return glm::vec3(colorSum / weightSum);
    }

    glm::vec3 WorldSystem::toLocal(const glm::dvec3& worldPos) const {
        // Restamos la posición de la cámara en alta precisión (double)
        // y el resultado lo bajamos a float para la GPU.
        return glm::vec3(worldPos - m_worldOrigin);
    }
}