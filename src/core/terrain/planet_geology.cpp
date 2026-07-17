#include "core/terrain/planet_geology.h"

#include <algorithm>
#include <cmath>

namespace Haruka {

namespace {

    // Hash entero determinista (mismo en cliente/servidor; no depende de <random>, que varía).
    inline uint32_t hashU(uint32_t x) {
        x ^= x >> 16; x *= 0x7feb352dU;
        x ^= x >> 15; x *= 0x846ca68bU;
        x ^= x >> 16;
        return x;
    }
    inline uint32_t hashC(uint32_t a, uint32_t b) {
        return hashU(a ^ (b + 0x9e3779b9U + (a << 6) + (a >> 2)));
    }
    inline float h01(uint32_t h) { return (float)(h & 0xFFFFFF) / (float)0xFFFFFF; }

    // --- Ruido de valor 3D (para DEFORMAR los bordes de placa) --------------------------------
    // Sin esto, un Voronoi sobre la esfera da límites de placa RECTOS (arcos de círculo máximo):
    // se ven artificiales al instante. Deformando la dirección ANTES de buscar la placa más cercana,
    // los bordes serpentean como los de verdad — y de paso las cordilleras dejan de ser rectas.
    inline float vhash(const glm::ivec3& p, uint32_t seed) {
        return h01(hashC(hashC((uint32_t)(p.x * 73856093) ^ (uint32_t)(p.y * 19349663),
                               (uint32_t)(p.z * 83492791)), seed)) * 2.0f - 1.0f;
    }
    float vnoise(const glm::vec3& x, uint32_t seed) {
        glm::vec3 i = glm::floor(x);
        glm::vec3 f = x - i;
        f = f * f * (3.0f - 2.0f * f);   // smoothstep
        glm::ivec3 I((int)i.x, (int)i.y, (int)i.z);
        auto L = [](float a, float b, float t) { return a + (b - a) * t; };
        float x00 = L(vhash(I + glm::ivec3(0,0,0), seed), vhash(I + glm::ivec3(1,0,0), seed), f.x);
        float x10 = L(vhash(I + glm::ivec3(0,1,0), seed), vhash(I + glm::ivec3(1,1,0), seed), f.x);
        float x01 = L(vhash(I + glm::ivec3(0,0,1), seed), vhash(I + glm::ivec3(1,0,1), seed), f.x);
        float x11 = L(vhash(I + glm::ivec3(0,1,1), seed), vhash(I + glm::ivec3(1,1,1), seed), f.x);
        return L(L(x00, x10, f.y), L(x01, x11, f.y), f.z);
    }
    // 2 octavas bastan: queremos ondular el borde, no romperlo.
    glm::vec3 warp(const glm::vec3& dir, uint32_t seed) {
        const float A = 0.16f;   // amplitud del serpenteo (rad aprox)
        glm::vec3 w(vnoise(dir * 2.3f, seed + 11u),
                    vnoise(dir * 2.3f, seed + 22u),
                    vnoise(dir * 2.3f, seed + 33u));
        glm::vec3 w2(vnoise(dir * 5.7f, seed + 44u),
                     vnoise(dir * 5.7f, seed + 55u),
                     vnoise(dir * 5.7f, seed + 66u));
        return glm::normalize(dir + (w * A + w2 * (A * 0.35f)));
    }

    inline float smoothstepf(float e0, float e1, float x) {
        float t = glm::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

} // namespace

void PlanetGeology::generate(uint32_t seed, int plateCount) {
    m_seed = seed;
    m_plates.clear();
    plateCount = std::clamp(plateCount, 6, 64);
    m_plates.reserve((size_t)plateCount);

    for (int i = 0; i < plateCount; ++i) {
        TectonicPlate p;
        const uint32_t h = hashC(seed, (uint32_t)i);

        // Sitio: distribución uniforme sobre la esfera (z uniforme + ángulo uniforme; si se usara
        // lat uniforme, las placas se apelotonarían en los polos).
        const float z   = 2.0f * h01(hashC(h, 1u)) - 1.0f;
        const float phi = 6.2831853f * h01(hashC(h, 2u));
        const float r   = std::sqrt(std::max(0.0f, 1.0f - z * z));
        p.site = glm::vec3(r * std::cos(phi), r * std::sin(phi), z);

        // Movimiento TANGENTE a la esfera en el sitio (una velocidad radial no significaría nada).
        glm::vec3 ref = (std::abs(p.site.y) < 0.9f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        glm::vec3 t1  = glm::normalize(glm::cross(ref, p.site));
        glm::vec3 t2  = glm::cross(p.site, t1);
        const float a = 6.2831853f * h01(hashC(h, 3u));
        p.speed  = 0.35f + 0.65f * h01(hashC(h, 4u));         // [0.35, 1]
        p.motion = (t1 * std::cos(a) + t2 * std::sin(a)) * p.speed;

        // Tipo de corteza. ~40% continental: en la Tierra la corteza continental cubre ~40% de la
        // superficie (de la que una parte está sumergida en las plataformas) → deja ~30% de tierra
        // emergida, que es la proporción que hace que un planeta "parezca" un planeta.
        p.continental = h01(hashC(h, 5u)) < 0.40f;

        m_plates.push_back(p);
    }
}

GeologySample PlanetGeology::sample(const glm::vec3& dirIn) const {
    GeologySample s;
    if (m_plates.empty()) return s;

    const glm::vec3 dir = glm::normalize(dirIn);
    // Bordes serpenteantes: buscamos la placa sobre la dirección DEFORMADA (ver warp()).
    const glm::vec3 d = warp(dir, m_seed);

    // Placa más cercana y segunda más cercana (distancia angular por producto escalar).
    int   i0 = 0, i1 = -1;
    float c0 = -2.0f, c1 = -2.0f;     // cosenos (mayor = más cerca)
    for (int i = 0; i < (int)m_plates.size(); ++i) {
        const float c = glm::dot(d, m_plates[i].site);
        if (c > c0)      { c1 = c0; i1 = i0; c0 = c; i0 = i; }
        else if (c > c1) { c1 = c;  i1 = i; }
    }
    if (i1 < 0) i1 = i0;

    const TectonicPlate& A = m_plates[i0];
    const TectonicPlate& B = m_plates[i1];

    // Distancia al LÍMITE: la frontera es la bisectriz entre los dos sitios, así que la diferencia
    // de distancias angulares (≈ la mitad) mide cuánto falta para el borde. En el borde vale 0.
    const float dA = std::acos(glm::clamp(c0, -1.0f, 1.0f));
    const float dB = std::acos(glm::clamp(c1, -1.0f, 1.0f));
    s.boundaryDist = std::max(0.0f, (dB - dA) * 0.5f);
    s.plateId      = i0;
    s.continental  = A.continental;

    // CONVERGENCIA: velocidad relativa proyectada sobre la normal del límite (la dirección que va
    // de A hacia B, hecha tangente). >0 = A avanza hacia B (chocan) · <0 = se separan.
    glm::vec3 toB = B.site - A.site;
    toB -= d * glm::dot(toB, d);                       // tangente en el punto
    const float toBLen = glm::length(toB);
    glm::vec3 nrm = (toBLen > 1e-6f) ? (toB / toBLen) : glm::vec3(0);
    const glm::vec3 rel = A.motion - B.motion;         // velocidad relativa
    s.convergence = glm::dot(rel, nrm);                // ~[-2, 2]

    // Peso del límite: 1 justo en el borde, se apaga a kBoundaryWidthRad. Es lo que hace que el
    // relieve tectónico sea una BANDA (cordillera) y no una mancha.
    const float w = 1.0f - smoothstepf(0.0f, kBoundaryWidthRad, s.boundaryDist);
    s.orogeny = w;

    // --- CONTINENTALIDAD: la costa NO es el borde de la placa ---------------------------------
    // Si la corteza fuera binaria por placa, la línea de costa sería EXACTAMENTE el polígono de la
    // placa: bordes limpios de Voronoi, que se leen como artificiales al instante. En la realidad la
    // corteza continental se ADELGAZA hacia el margen (margen pasivo) y el mar la invade de forma
    // irregular → plataforma continental, bahías, penínsulas e islas.
    //
    // Modelo: continentalidad ∈ [0,1] = mezcla suave entre las dos placas del límite + ruido. La
    // costa emerge de dónde ese campo cruza el umbral → contorno FRACTAL, no un polígono.
    const float contA = A.continental ? 1.0f : 0.0f;
    const float contB = B.continental ? 1.0f : 0.0f;
    // Peso de la placa vecina: 0.5 justo en el borde → 0 tierra adentro (margen ancho: ~kMarginRad).
    const float bw    = 0.5f * (1.0f - smoothstepf(0.0f, kMarginRad, s.boundaryDist));
    float continentality = contA * (1.0f - bw) + contB * bw;
    // Ruido en el margen: rompe la línea. Amplitud MÁXIMA en la costa y nula tierra/mar adentro
    // (si no, aparecerían lagos secos en mitad del continente y arrecifes en la llanura abisal).
    const float edgeW = 1.0f - std::abs(continentality * 2.0f - 1.0f);   // 1 en la transición, 0 en los extremos
    continentality += (vnoise(dir * 6.0f, m_seed + 909u) * 0.28f
                     + vnoise(dir * 14.0f, m_seed + 313u) * 0.12f) * edgeW;

    // Umbral → tierra/mar. La transición NO es un escalón: entre medias está la PLATAFORMA
    // continental (corteza adelgazada, aún sumergida) — el bajío que hace creíble una costa.
    const float land = smoothstepf(0.42f, 0.62f, continentality);
    s.continental    = (land > 0.5f);

    // --- Elevación base de la corteza ---------------------------------------------------------
    // Interpolamos ENTRE lecho oceánico y corteza continental: los valores intermedios son
    // literalmente la plataforma + el talud continental.
    float elev = kOceanicBaseKm + (kContinentalBaseKm - kOceanicBaseKm) * land;

    // El TIPO de choque se decide con la corteza REAL de este punto (la suavizada), no con la
    // etiqueta de la placa: si no, un arco volcánico podría brotar en mitad del mar.
    const bool  bothCont = s.continental && (contB > 0.5f);
    const bool  bothOcea = !s.continental && (contB <= 0.5f);
    const float conv     = s.convergence;

    if (conv > 0.0f) {
        // --- CONVERGENTE: chocan ---
        const float f = conv * w;
        if (bothCont) {
            // Colisión continental → CORDILLERA. El perfil (w²) concentra la altura en el eje y deja
            // faldas largas: es lo que hace que se lea como cadena y no como pared.
            elev += kMaxOrogenyKm * f * w;
        } else if (bothOcea) {
            // Océano-océano → FOSA + ARCO DE ISLAS (la placa más lenta cabalga a la que subduce).
            const bool overriding = A.speed <= B.speed;
            elev += overriding ? (3.2f * f * w)      // arco de islas (puede asomar sobre el mar)
                               : (-4.5f * f);        // fosa
        } else {
            // Continente-océano → la oceánica SUBDUCE: fosa en el mar, arco volcánico en tierra.
            elev += s.continental ? (4.6f * f * w)   // arco volcánico (Andes)
                                  : (-5.0f * f);     // fosa
        }
    } else if (conv < 0.0f) {
        // --- DIVERGENTE: se separan ---
        const float f = (-conv) * w;
        if (bothCont) {
            elev -= 1.6f * f;                        // VALLE DE RIFT (se hunde la corteza)
        } else {
            elev += 2.4f * f;                        // DORSAL oceánica (lecho joven y elevado)
        }
    }
    // (Transformante: convergence ≈ 0 → apenas relieve. Correcto: las fallas no levantan montañas.)

    s.upliftKm = elev;
    return s;
}

} // namespace Haruka
