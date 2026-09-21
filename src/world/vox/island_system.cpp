/**
 * @file island_system.cpp
 * @brief Siembra, horneado a mapas y muestreo de las islas flotantes. Ver `island_system.h`.
 */
#include "world/vox/island_system.h"
#include "world/terrain/bake_util.h"

#include "core/logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace Haruka {

using namespace BakeUtil;

namespace {
inline int cellsPerFace() { return (int)std::lround((3.14159265358979 * 0.5) / kIslandCellRad); }
} // namespace

// ── IslandBox ───────────────────────────────────────────────────────────────────────────────────

glm::vec3 IslandBox::toLocal(const glm::dvec3& p) const {
    // La altura es RADIAL (`|p| − baseRM`), no la proyección sobre la normal del centro: sobre la
    // esfera la base es un casquete, y así la isla no se despega de su altitud hacia los bordes
    // (misma decisión que `CaveBox::toLocal`).
    const double r = glm::length(p);
    const glm::dvec3 rel = p - centerDir * baseRM;
    return glm::vec3((float)glm::dot(rel, tangentU), (float)glm::dot(rel, tangentV), (float)(r - baseRM));
}

glm::dvec3 IslandBox::toWorld(const glm::vec3& l) const {
    return centerDir * (baseRM + (double)l.z) + tangentU * (double)l.x + tangentV * (double)l.y;
}

bool IslandBox::contains(const glm::dvec3& p) const {
    const glm::vec3 l = toLocal(p);
    return std::fabs(l.x) <= radiusM && std::fabs(l.y) <= radiusM
        && l.z >= -rootM - kIslandRangeM && l.z <= topM + kIslandRangeM;
}

// ── Siembra ─────────────────────────────────────────────────────────────────────────────────────

static IslandBox makeBox(uint32_t seed, int face, int i, int j, int n, double planetRadiusM,
                         float (*surfaceElevM)(const glm::dvec3&, void*), void* user) {
    IslandBox b;
    b.seed = seed;
    b.cell = cellKey(face, i, j, n);
    const double cu = -1.0 + (2.0 * ((double)i + 0.5) / (double)n);
    const double cv = -1.0 + (2.0 * ((double)j + 0.5) / (double)n);
    const double jit = 2.0 / (double)n * 0.3;
    const float r1 = hash01(b.cell, seed ^ 0x51ed2711u), r2 = hash01(b.cell, seed ^ 0x8e3779b1u);
    b.centerDir = faceUVToDir(face, cu + (r1 - 0.5f) * 2.0 * jit, cv + (r2 - 0.5f) * 2.0 * jit);
    const glm::dvec3 ref = (std::fabs(b.centerDir.y) < 0.99) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    b.tangentU = glm::normalize(glm::cross(ref, b.centerDir));
    b.tangentV = glm::cross(b.centerDir, b.tangentU);
    // Tamaño: cajas de 200-350 m de medio lado (la silueta ocupa ~la mitad), cima de 15-50 m de
    // relieve, raíz de 60-160 m, y flota a 150-320 m sobre la cota del terreno en su centro.
    b.radiusM   = 200.0f + 150.0f * hash01(b.cell, seed ^ 0x2545f491u);
    b.topM      =  15.0f +  35.0f * hash01(b.cell, seed ^ 0xc2b2ae35u);
    b.rootM     =  60.0f + 100.0f * hash01(b.cell, seed ^ 0x27d4eb2fu);
    b.altitudeM = 150.0f + 170.0f * hash01(b.cell, seed ^ 0x165667b1u);
    const float elev = surfaceElevM ? surfaceElevM(b.centerDir, user) : 0.0f;
    b.baseRM = planetRadiusM + (double)elev + (double)b.altitudeM;
    return b;
}

static float g_islandAutoProb = 0.0f;
void  islandSetAutoProbability(float p) { g_islandAutoProb = std::clamp(p, 0.0f, 1.0f); }
float islandAutoProbability() { return g_islandAutoProb; }

void islandBoxForCell(uint32_t seed, const glm::dvec3& dir, double planetRadiusM, IslandBox& out,
                      float (*surfaceElevM)(const glm::dvec3&, void*), void* user) {
    const int n = cellsPerFace();
    int face, i, j;
    cellOf(dir, n, face, i, j);
    out = makeBox(seed, face, i, j, n, planetRadiusM, surfaceElevM, user);
}

bool islandBoxAt(uint32_t seed, const glm::dvec3& dir, double planetRadiusM, IslandBox& out,
                 float (*surfaceElevM)(const glm::dvec3&, void*), void* user) {
    if (planetRadiusM <= 0.0 || g_islandAutoProb <= 0.0f) return false;
    const int n = cellsPerFace();
    int face, i, j;
    cellOf(dir, n, face, i, j);
    if (hash01(cellKey(face, i, j, n), seed ^ 0x151a4d00u) > g_islandAutoProb) return false;
    out = makeBox(seed, face, i, j, n, planetRadiusM, surfaceElevM, user);
    return true;
}

// ── Islas de la escena (sin semilla) ────────────────────────────────────────────────────────────

IslandBox islandBoxFromDef(const IslandDef& def, double planetRadiusM,
                           float (*surfaceElevM)(const glm::dvec3&, void*), void* user) {
    IslandBox b;
    b.seed = def.draftSeed;
    b.cell = caveIdFromName(def.name);
    b.centerDir = glm::normalize(def.centerDir);
    const glm::dvec3 ref = (std::fabs(b.centerDir.y) < 0.99) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    b.tangentU = glm::normalize(glm::cross(ref, b.centerDir));
    b.tangentV = glm::cross(b.centerDir, b.tangentU);
    b.radiusM   = std::max(def.radiusM, 20.0f);
    b.topM      = std::max(def.topM, 1.0f);
    b.rootM     = std::max(def.rootM, 1.0f);
    b.altitudeM = def.altitudeM;
    const float elev = surfaceElevM ? surfaceElevM(b.centerDir, user) : 0.0f;
    b.baseRM = planetRadiusM + (double)elev + (double)b.altitudeM;
    return b;
}

bool islandLoad(const IslandDef& def, IslandSystem& out, double planetRadiusM, int mapRes,
                float (*surfaceElevM)(const glm::dvec3&, void*), void* user) {
    return islandLoadBox(def, islandBoxFromDef(def, planetRadiusM, surfaceElevM, user), out, mapRes);
}

bool islandLoadBox(const IslandDef& def, const IslandBox& box, IslandSystem& out, int mapRes) {
    out = IslandSystem{};
    out.box = box;
    int w = 0, h = 0; std::vector<float> px;
    auto load = [&](const std::string& path, CaveMap& m) {
        const std::string p = caveResolveAssetPath(path);
        if (p.empty() || !loadPNGGray(p, w, h, px)) return false;
        m.w = w; m.h = h; m.v = px; return true;
    };
    const bool ok = load(def.planta, out.planta) && load(def.cima, out.cima) && load(def.raiz, out.raiz);
    if (!ok) {
        islandBakeMaps(out.box, mapRes, out.planta, out.cima, out.raiz, &out.stats);
        auto save = [&](const std::string& path, const CaveMap& m) {
            const std::string p = caveResolveAssetPath(path);
            if (!p.empty()) savePNGGray(p, m.w, m.h, m.v);
        };
        save(def.planta, out.planta); save(def.cima, out.cima); save(def.raiz, out.raiz);
        HARUKA_LOGI("Islas", "'%s': sin mapas -> borrador (semilla %u)%s", def.name.c_str(), def.draftSeed,
                    def.planta.empty() ? " (no se guarda: sin ruta)" : ", guardado como asset");
    }
    return out.valid();
}

// ── Horneado ────────────────────────────────────────────────────────────────────────────────────

void islandBakeMaps(const IslandBox& box, int res, CaveMap& planta, CaveMap& cima, CaveMap& raiz,
                    glm::ivec3* outStats) {
    res = std::max(8, res);
    planta.w = planta.h = cima.w = cima.h = raiz.w = raiz.h = res;
    planta.v.assign((size_t)res * res, 0.0f);
    cima.v.assign((size_t)res * res, 0.0f);
    raiz.v.assign((size_t)res * res, 0.0f);
    const uint32_t s = box.seed ^ (box.cell * 2654435761u);
    const float texelM = 2.0f * box.radiusM / (float)(res - 1);

    // SILUETA: una mancha de fBm apagada hacia el borde de la caja (la isla no puede salirse de su
    // caja: el vecino no sabe que existe). Umbral fijo → la silueta ocupa ~35-55 % de la caja.
    const float freq = 1.6f + 1.2f * hash01(box.cell, s ^ 0x1111u);
    std::vector<uint8_t> inside((size_t)res * res, 0);
    for (int y = 0; y < res; ++y)
        for (int x = 0; x < res; ++x) {
            const float u = (float)x / (float)(res - 1), t = (float)y / (float)(res - 1);
            const float dx = (u - 0.5f) * 2.0f, dy = (t - 0.5f) * 2.0f;
            const float rr = std::sqrt(dx * dx + dy * dy);
            float m = fbm2(u * freq + 3.1f, t * freq + 5.7f, s ^ 0x2222u, 4);
            m = m * 0.6f + 0.4f * (1.0f - rr);                     // la mancha, centrada
            m *= 1.0f - std::clamp((rr - 0.70f) / 0.25f, 0.0f, 1.0f);   // nunca toca el borde de la caja
            inside[(size_t)y * res + x] = (m > 0.52f) ? 1 : 0;
        }
    // UNA PIEZA: una isla con dos cachos separados serían dos islas (y la pequeña, un pedrusco
    // colgado). Se mide cuántas había: es el dato de si el fBm necesitaba post-proceso.
    const int comps = keepLargestComponent2D(res, res, inside);
    int texels = 0;
    for (uint8_t v : inside) texels += v;

    // Distancia con signo al borde (m), saturada a ±kIslandRangeM, a [0,1] con 0,5 en el borde.
    std::vector<float> sd;
    chamferSigned2D(res, res, inside, sd);
    for (size_t i = 0; i < sd.size(); ++i) {
        const float m = sd[i] * texelM;
        planta.v[i] = std::clamp(m / (2.0f * kIslandRangeM) + 0.5f, 0.0f, 1.0f);
    }

    // CIMA: un trozo de mundo arrancado — relieve de colinas (fBm) sobre una base que nunca baja
    // de 0,3·topM, HASTA EL BORDE: el acantilado lo pone la distancia lateral de la planta. (Una
    // primera versión apagaba la cima en los últimos 6 m y salía una TERRAZA a la altura de la base
    // rodeando la meseta — visto en el juego como un escalón claro alrededor del borde.)
    // RAÍZ: un cono dentado: más honda cuanto más lejos del borde, con ridged para las aristas.
    const float fTop = 2.0f + 2.0f * hash01(box.cell, s ^ 0x3333u);
    for (int y = 0; y < res; ++y)
        for (int x = 0; x < res; ++x) {
            const size_t i = (size_t)y * res + x;
            const float u = (float)x / (float)(res - 1), t = (float)y / (float)(res - 1);
            const float edgeM = std::max(sd[i] * texelM, 0.0f);   // metros hacia dentro del borde
            const float hills = fbm2(u * fTop + 9.2f, t * fTop + 1.4f, s ^ 0x4444u, 5);
            const float top = 0.3f + 0.7f * hills;
            cima.v[i] = std::clamp(top, 0.0f, 1.0f);
            const float in = std::clamp(edgeM / (0.55f * box.radiusM), 0.0f, 1.0f);
            const float teeth = 0.65f + 0.35f * ridged2(u * 4.0f + 2.2f, t * 4.0f + 8.8f, s ^ 0x5555u, 3);
            raiz.v[i] = std::clamp(std::pow(in, 0.7f) * teeth, 0.0f, 1.0f);
        }
    if (outStats) *outStats = glm::ivec3(comps, texels, 0);
}

std::string islandCacheKey(const IslandBox& box, int mapRes) {
    uint64_t h = 1469598103934665603ull;
    const char* version = "island-bake-v2";   // v2: la cima llega al borde (sin terraza)
    mix(h, version, std::strlen(version));
    mix(h, &box.seed, sizeof(box.seed));
    mix(h, &box.cell, sizeof(box.cell));
    mix(h, &box.radiusM, sizeof(box.radiusM));
    mix(h, &box.topM, sizeof(box.topM));
    mix(h, &box.rootM, sizeof(box.rootM));
    mix(h, &box.baseRM, sizeof(box.baseRM));
    mix(h, &mapRes, sizeof(mapRes));
    return hex16(h);
}

bool islandLoadOrBake(const IslandBox& box, IslandSystem& out, const std::string& dir, int mapRes) {
    out.box = box;
    const std::string base = bakesDir(dir) + "island_" + islandCacheKey(box, mapRes);
    int w = 0, h = 0;
    std::vector<float> px;
    auto load = [&](const char* suffix, CaveMap& m) {
        if (!loadPNGGray(base + suffix, w, h, px)) return false;
        m.w = w; m.h = h; m.v = px; return true;
    };
    if (load("_planta.png", out.planta) && load("_cima.png", out.cima) && load("_raiz.png", out.raiz)) {
        out.stats = glm::ivec3(0, 0, 0);
        return true;
    }
    const auto t0 = std::chrono::high_resolution_clock::now();
    islandBakeMaps(box, mapRes, out.planta, out.cima, out.raiz, &out.stats);
    savePNGGray(base + "_planta.png", out.planta.w, out.planta.h, out.planta.v);
    savePNGGray(base + "_cima.png", out.cima.w, out.cima.h, out.cima.v);
    savePNGGray(base + "_raiz.png", out.raiz.w, out.raiz.h, out.raiz.v);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
    HARUKA_LOGI("Islas", "celda %u horneada en %.0f ms: %d componentes de silueta -> 1 · %d texels de isla",
                box.cell, ms, out.stats.x, out.stats.y);
    return out.valid();
}

// ── Muestreo ────────────────────────────────────────────────────────────────────────────────────

static inline bool localUV(const IslandSystem& sys, const glm::vec3& l, float& u, float& v) {
    u = (l.x / sys.box.radiusM) * 0.5f + 0.5f;
    v = (l.y / sys.box.radiusM) * 0.5f + 0.5f;
    // Un texel de holgura, por lo mismo que en `caveDensityLocal` (curvatura + ulp del radio).
    const float tol = 1.0f / (float)std::max(sys.planta.w - 1, 1);
    if (u < -tol || u > 1.0f + tol || v < -tol || v > 1.0f + tol) return false;
    u = std::clamp(u, 0.0f, 1.0f); v = std::clamp(v, 0.0f, 1.0f);
    return true;
}

float islandDensityAt(const IslandSystem& sys, const glm::dvec3& p) {
    if (!sys.valid()) return -1e9f;
    const glm::vec3 l = sys.box.toLocal(p);
    float u, v;
    if (!localUV(sys, l, u, v)) return -1e9f;
    if (l.z > sys.box.topM + kIslandRangeM || l.z < -sys.box.rootM - kIslandRangeM) return -1e9f;
    const float lat = (sys.planta.at(u, v) - 0.5f) * 2.0f * kIslandRangeM;   // + dentro de la silueta
    const float top =  sys.box.topM  * sys.cima.at(u, v);
    const float bot = -sys.box.rootM * sys.raiz.at(u, v);
    // Intersección de tres medios espacios: dentro de la silueta, bajo la cima, sobre la raíz.
    return std::min(lat, std::min(top - l.z, l.z - bot));
}

bool islandColumnAt(const IslandSystem& sys, const glm::dvec3& dir, float& topH, float& bottomH) {
    if (!sys.valid()) return false;
    const glm::vec3 l = sys.box.toLocal(glm::normalize(dir) * sys.box.baseRM);
    float u, v;
    if (!localUV(sys, l, u, v)) return false;
    if (sys.planta.at(u, v) <= 0.5f) return false;
    topH    =  sys.box.topM  * sys.cima.at(u, v);
    bottomH = -sys.box.rootM * sys.raiz.at(u, v);
    return true;
}

} // namespace Haruka
