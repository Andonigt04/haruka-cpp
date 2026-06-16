#include "core/terrain/floating_islands.h"
#include "core/terrain/terrain_sampler_v2.h"
#include "core/noise_generator.h"

#include <cmath>
#include <algorithm>

namespace Haruka {
namespace {

inline uint32_t uhash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
inline uint32_t uhash2(uint32_t a, uint32_t b) { return uhash(a ^ (uhash(b) + 0x9e3779b9u)); }
inline float hash01(uint32_t x) { return float(uhash(x) & 0xFFFFFFu) / float(0x1000000); }

constexpr float PI = 3.14159265358979f;

} // namespace

FloatingIslandMesh generateIslandMesh(float radius, uint32_t seed, int sectors, int stacks) {
    FloatingIslandMesh m;
    const int vcols = sectors + 1;
    m.vertices.reserve((size_t)(stacks + 1) * vcols);

    // --- Vértices: esfera UV deformada a forma de isla flotante ---
    for (int i = 0; i <= stacks; ++i) {
        float phi = PI * float(i) / float(stacks);      // 0 (arriba) … PI (abajo)
        float cy = std::cos(phi);                        // y en [-1,1] (arriba→abajo)
        float sy = std::sin(phi);
        for (int j = 0; j <= sectors; ++j) {
            float th = 2.0f * PI * float(j) / float(sectors);
            glm::vec3 dir(sy * std::cos(th), cy, sy * std::sin(th)); // unit
            float y = dir.y;

            // Forma: copa achatada arriba, fondo cónico que se afila a un punto.
            float top    = (y >= 0.0f) ? glm::mix(1.0f, 0.72f, y) : 1.0f; // achatar copa
            float bottom = (y <  0.0f) ? std::pow(1.0f + y, 0.55f) : 1.0f; // afilar fondo
            // Irregularidad de silueta + raíces dentadas en el fondo.
            float noise  = NoiseGenerator::fBm(dir * 2.5f, (int)seed, 4, 0.5f, 2.0f, 1.0f) * 0.18f;
            float spikes = (y < 0.0f)
                ? 0.32f * (-y) * std::fabs(NoiseGenerator::fBm(dir * 5.0f, (int)seed + 7, 3, 0.5f, 2.0f, 1.0f))
                : 0.0f;
            float shape = top * bottom * (1.0f + noise) + spikes;

            glm::vec3 v = dir * (radius * shape);
            v.y *= (y >= 0.0f) ? 0.6f : 1.7f;            // copa plana, fondo colgante
            m.vertices.push_back(v);
        }
    }

    // --- Índices (triangulación UV) ---
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < sectors; ++j) {
            int a = i * vcols + j;
            int b = a + vcols;
            m.indices.push_back(a);     m.indices.push_back(b);     m.indices.push_back(a + 1);
            m.indices.push_back(a + 1); m.indices.push_back(b);     m.indices.push_back(b + 1);
        }
    }

    // --- Normales por promedio de normales de cara ---
    m.normals.assign(m.vertices.size(), glm::vec3(0.0f));
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        unsigned int ia = m.indices[t], ib = m.indices[t + 1], ic = m.indices[t + 2];
        glm::vec3 fn = glm::cross(m.vertices[ib] - m.vertices[ia], m.vertices[ic] - m.vertices[ia]);
        m.normals[ia] += fn; m.normals[ib] += fn; m.normals[ic] += fn;
    }
    for (auto& n : m.normals) {
        float l = glm::length(n);
        n = (l > 1e-12f) ? n / l : glm::vec3(0, 1, 0);
    }
    return m;
}

std::vector<FloatingIsland> generateFloatingIslands(uint32_t seed,
                                                    const glm::dvec3& planetCenter,
                                                    double planetRadius,
                                                    const WorldGenParams& W) {
    std::vector<FloatingIsland> out;
    const int   CAND = 320;          // candidatos repartidos (esfera de Fibonacci)
    const float ga   = 2.39996323f;
    const float islandProb = 0.08f;  // raras: ~8% de candidatos
    const double kmToFrac  = 1000.0 / planetRadius;

    for (int i = 0; i < CAND; ++i) {
        uint32_t h = uhash((uint32_t)i ^ (seed * 2654435761u));
        if (hash01(h ^ 0xF10A7u) > islandProb) continue; // gate de existencia

        float y = 1.0f - 2.0f * (i + 0.5f) / CAND;
        float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        float th = ga * i;
        glm::vec3 dir(std::cos(th) * r, y, std::sin(th) * r);

        TerrainSample ts = sampleTerrainV2(dir, W, planetRadius);
        if (ts.landMask < 0.5f) continue;                 // solo sobre tierra

        float radius   = glm::mix(40.0f, 200.0f, hash01(h ^ 0x5A5Au));   // 40–200 m
        float altitude = glm::mix(300.0f, 1200.0f, hash01(h ^ 0x3C3Cu)); // flota 0.3–1.2 km sobre el suelo

        double surfaceR = planetRadius * (1.0 + double(ts.elevKm) * kmToFrac);
        FloatingIsland isl;
        isl.worldCenter = planetCenter + glm::dvec3(dir) * (surfaceR + double(altitude));
        isl.mesh   = generateIslandMesh(radius, h);
        isl.radius = radius * 2.0f; // acotador holgado (incluye raíces)
        out.push_back(std::move(isl));
    }
    return out;
}

std::vector<FloatingIsland> generateFloatingIslandsNear(uint32_t seed,
                                                        const glm::dvec3& planetCenter,
                                                        double planetRadius,
                                                        const WorldGenParams& W,
                                                        const glm::dvec3& camDir,
                                                        float angRadius) {
    const float  density  = 160.0f;   // celdas de ~40 km sobre la esfera
    const float  prob     = 0.50f;    // fracción de celdas con isla
    const double kmToFrac = 1000.0 / planetRadius;
    const float  cosR     = std::cos(angRadius);
    const size_t MAX_ISL  = 48;        // cota de seguridad (mallas/draws)

    glm::vec3 cd = glm::normalize(glm::vec3(camDir));
    glm::ivec3 base = glm::ivec3(glm::floor(cd * density));
    int span = (int)std::ceil(angRadius * density) + 1;

    // 1) Recoger candidatos (dir + atributos) en el casquete, sobre tierra.
    struct Cand { glm::vec3 dir; uint32_t ch; float dot; };
    std::vector<Cand> cands;
    for (int dk = -span; dk <= span; ++dk)
    for (int dj = -span; dj <= span; ++dj)
    for (int di = -span; di <= span; ++di) {
        glm::ivec3 cell = base + glm::ivec3(di, dj, dk);
        uint32_t ch = uhash2((uint32_t)(cell.x * 73856093) ^ (uint32_t)(cell.y * 19349663)
                                                          ^ (uint32_t)(cell.z * 83492791), seed);
        if (hash01(ch ^ 0x151A4Du) > prob) continue;
        glm::vec3 jit(float(uhash(ch + 1) & 0xFFFF) / 65535.0f,
                      float(uhash(ch + 2) & 0xFFFF) / 65535.0f,
                      float(uhash(ch + 3) & 0xFFFF) / 65535.0f);
        glm::vec3 cdir = glm::normalize(glm::vec3(cell) + jit);
        float dt = glm::dot(cd, cdir);
        if (dt < cosR) continue;                              // fuera del casquete
        if (sampleTerrainV2(cdir, W, planetRadius).landMask < 0.5f) continue; // solo tierra
        cands.push_back({ cdir, ch, dt });
    }

    // 2) Más CERCANAS primero (mayor dot) y recortar a la cota → islas cerca de ti.
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b){ return a.dot > b.dot; });
    if (cands.size() > MAX_ISL) cands.resize(MAX_ISL);

    // 3) Construir las mallas (lo caro) solo para las que sobreviven.
    std::vector<FloatingIsland> out;
    out.reserve(cands.size());
    for (const auto& c : cands) {
        TerrainSample ts = sampleTerrainV2(c.dir, W, planetRadius);
        float radius   = glm::mix(40.0f, 200.0f, hash01(c.ch ^ 0x5A5Au));
        float altitude = glm::mix(300.0f, 1200.0f, hash01(c.ch ^ 0x3C3Cu));
        double surfaceR = planetRadius * (1.0 + double(ts.elevKm) * kmToFrac);
        FloatingIsland isl;
        isl.worldCenter = planetCenter + glm::dvec3(c.dir) * (surfaceR + double(altitude));
        isl.mesh   = generateIslandMesh(radius, c.ch);
        isl.radius = radius * 2.0f;
        out.push_back(std::move(isl));
    }
    return out;
}

} // namespace Haruka
