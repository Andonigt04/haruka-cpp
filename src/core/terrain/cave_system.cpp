/**
 * @file cave_system.cpp
 * @brief Siembra, horneado a mapas, conectividad y muestreo de las cuevas. Ver `cave_system.h`.
 */
#include "core/terrain/cave_system.h"
#include "core/terrain/bake_util.h"

#include "core/asset_paths.h"
#include "core/logger.h"
#include "io/image_writer.h"

#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <numeric>

namespace Haruka {

using namespace BakeUtil;

namespace BakeUtil {

std::string bakesDir(const std::string& dir) {
    if (!dir.empty()) return (dir.back() == '/' || dir.back() == '\\') ? dir : dir + "/";
    const std::string& root = AssetPaths::projectRoot();
    return root.empty() ? std::string("bakes/") : root + "bakes/";
}

// PNG gris de 8 bits, ida y vuelta. El escritor del motor sólo hace RGB/RGBA y gris de 16, así que
// el gris de 8 va como RGBA con los tres canales iguales: se sigue viendo como una imagen normal en
// cualquier visor —que es medio motivo de tenerlo en un fichero— y comprime igual de bien.
bool savePNGGray(const std::string& path, int w, int h, const std::vector<float>& v) {
    if (w <= 0 || h <= 0 || (int)v.size() != w * h) return false;
    std::vector<unsigned char> px((size_t)w * h * 4);
    for (size_t i = 0; i < v.size(); ++i) {
        const unsigned char g = (unsigned char)std::lround(std::clamp(v[i], 0.0f, 1.0f) * 255.0f);
        px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = g;
        px[i * 4 + 3] = 255;
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    return writePNG(path, w, h, 4, px.data());
}

bool loadPNGGray(const std::string& path, int& w, int& h, std::vector<float>& v) {
    int n = 0;
    unsigned char* p = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!p) return false;
    v.resize((size_t)w * h);
    for (size_t i = 0; i < v.size(); ++i) v[i] = (float)p[i * 4] / 255.0f;
    stbi_image_free(p);
    return true;
}

void chamferSigned2D(int w, int h, const std::vector<uint8_t>& inside, std::vector<float>& out) {
    // Distancia al texel más cercano del OTRO medio, por separado para dentro y fuera, y se firma.
    const float BIG = 1e9f;
    std::vector<float> dIn((size_t)w * h, BIG), dOut((size_t)w * h, BIG);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t)y * w + x;
            // Frontera: un texel es "semilla" de la distancia del medio contrario.
            (inside[i] ? dOut : dIn)[i] = 0.0f;
        }
    auto pass = [&](std::vector<float>& d) {
        auto relax = [&](int x, int y, int dx, int dy, float c) {
            const int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) return;
            float& v = d[(size_t)y * w + x];
            v = std::min(v, d[(size_t)ny * w + nx] + c);
        };
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) { relax(x, y, -1, 0, 3); relax(x, y, 0, -1, 3); relax(x, y, -1, -1, 4); relax(x, y, 1, -1, 4); }
        for (int y = h - 1; y >= 0; --y)
            for (int x = w - 1; x >= 0; --x) { relax(x, y, 1, 0, 3); relax(x, y, 0, 1, 3); relax(x, y, 1, 1, 4); relax(x, y, -1, 1, 4); }
    };
    pass(dIn); pass(dOut);
    out.resize((size_t)w * h);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = (inside[i] ? dIn[i] : -dOut[i]) / 3.0f;
}

int keepLargestComponent2D(int w, int h, std::vector<uint8_t>& inside) {
    std::vector<int> label((size_t)w * h, -1);
    std::vector<int> size;
    std::vector<int> stack;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i0 = (size_t)y * w + x;
            if (!inside[i0] || label[i0] >= 0) continue;
            const int id = (int)size.size();
            size.push_back(0);
            stack.assign(1, (int)i0); label[i0] = id;
            while (!stack.empty()) {
                const int i = stack.back(); stack.pop_back();
                ++size[id];
                const int cx = i % w, cy = i / w;
                const int nb[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
                for (auto& d : nb) {
                    const int nx = cx + d[0], ny = cy + d[1];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const size_t j = (size_t)ny * w + nx;
                    if (inside[j] && label[j] < 0) { label[j] = id; stack.push_back((int)j); }
                }
            }
        }
    if (size.size() <= 1) return (int)size.size();
    int best = 0;
    for (size_t i = 1; i < size.size(); ++i) if (size[i] > size[best]) best = (int)i;
    for (size_t i = 0; i < inside.size(); ++i) if (inside[i] && label[i] != best) inside[i] = 0;
    return (int)size.size();
}

} // namespace BakeUtil

namespace {
inline int cellsPerFace() {
    return (int)std::lround((3.14159265358979 * 0.5) / kCaveCellRad);
}
} // namespace

// ── CaveBox ─────────────────────────────────────────────────────────────────────────────────────

glm::vec3 CaveBox::toLocal(const glm::dvec3& p) const {
    const double r = glm::length(p);
    // Profundidad = cuánto por debajo del techo de la caja. Sobre una esfera el "techo" es el
    // casquete a `surfaceRM`, no un plano: a 400 m del centro de la caja la curvatura son 1,3 cm en
    // la Tierra, o sea nada, pero usar el radio (y no la proyección sobre la normal del centro)
    // evita que la caja se despegue del suelo en un planeta pequeño, donde sí importa.
    const glm::dvec3 rel = p - centerDir * surfaceRM;
    return glm::vec3((float)glm::dot(rel, tangentU), (float)glm::dot(rel, tangentV),
                     (float)(surfaceRM - r));
}

glm::dvec3 CaveBox::toWorld(const glm::vec3& l) const {
    return centerDir * (surfaceRM - (double)l.z)
         + tangentU * (double)l.x + tangentV * (double)l.y;
}

bool CaveBox::contains(const glm::dvec3& p) const {
    const glm::vec3 l = toLocal(p);
    return std::fabs(l.x) <= radiusM && std::fabs(l.y) <= radiusM && l.z >= 0.0f && l.z <= depthM;
}

// ── CaveMap ─────────────────────────────────────────────────────────────────────────────────────

float CaveMap::at(float u, float t) const {
    if (v.empty()) return 0.0f;
    const float fx = std::clamp(u, 0.0f, 1.0f) * (float)(w - 1);
    const float fy = std::clamp(t, 0.0f, 1.0f) * (float)(h - 1);
    const int x0 = (int)fx, y0 = (int)fy;
    const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float ax = fx - (float)x0, ay = fy - (float)y0;
    const float a = v[(size_t)y0 * w + x0], b = v[(size_t)y0 * w + x1];
    const float c = v[(size_t)y1 * w + x0], d = v[(size_t)y1 * w + x1];
    return (a + (b - a) * ax) + ((c + (d - c) * ax) - (a + (b - a) * ax)) * ay;
}

// ── Siembra ─────────────────────────────────────────────────────────────────────────────────────

static CaveBox makeBox(uint32_t seed, int face, int i, int j, int n, double planetRadiusM,
                       float (*surfaceElevM)(const glm::dvec3&, void*), void* user) {
    CaveBox b;
    b.seed = seed;
    b.cell = cellKey(face, i, j, n);
    // Centro de la celda, con un jitter determinista dentro de ella (si no, las bocas caen en una
    // rejilla perfecta y se nota a simple vista desde el aire).
    const double cu = -1.0 + (2.0 * ((double)i + 0.5) / (double)n);
    const double cv = -1.0 + (2.0 * ((double)j + 0.5) / (double)n);
    const double jit = 2.0 / (double)n * 0.35;
    const float r1 = hash01(b.cell, seed ^ 0x51ed2701u), r2 = hash01(b.cell, seed ^ 0x9e3779b9u);
    b.centerDir = faceUVToDir(face, cu + (r1 - 0.5f) * 2.0 * jit, cv + (r2 - 0.5f) * 2.0 * jit);

    // Base tangente determinista (la misma construcción de siempre: el `cross` degenera cerca del
    // polo del eje de referencia, así que se cambia de eje).
    const glm::dvec3 ref = (std::fabs(b.centerDir.y) < 0.99) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    b.tangentU = glm::normalize(glm::cross(ref, b.centerDir));
    b.tangentV = glm::cross(b.centerDir, b.tangentU);

    // Tamaño por celda (§A.1 del plan: Dmax ~80-300 m, Rc ~100-400 m).
    b.radiusM = 150.0f + 250.0f * hash01(b.cell, seed ^ 0x2545f491u);
    b.depthM  =  80.0f + 220.0f * hash01(b.cell, seed ^ 0xc2b2ae35u);
    // La boca del borrador, de la semilla (una cueva de la escena la trae escrita).
    const uint32_t s = seed ^ (b.cell * 2654435761u);
    b.mouth = glm::vec3(0.25f + 0.5f * hash01(b.cell, s ^ 0x7777u), 0.25f + 0.5f * hash01(b.cell, s ^ 0x8888u),
                        0.045f + 0.03f * hash01(b.cell, s ^ 0x9999u));
    const float elev = surfaceElevM ? surfaceElevM(b.centerDir, user) : 0.0f;
    b.surfaceRM = planetRadiusM + (double)elev;
    return b;
}

static float g_caveAutoProb = 0.0f;
void  caveSetAutoProbability(float p) { g_caveAutoProb = std::clamp(p, 0.0f, 1.0f); }
float caveAutoProbability() { return g_caveAutoProb; }

void caveBoxForCell(uint32_t seed, const glm::dvec3& dir, double planetRadiusM, CaveBox& out,
                    float (*surfaceElevM)(const glm::dvec3&, void*), void* user) {
    const int n = cellsPerFace();
    int face; double u, v;
    dirToFaceUV(glm::normalize(dir), face, u, v);
    const int i = std::clamp((int)std::floor((u + 1.0) * 0.5 * n), 0, n - 1);
    const int j = std::clamp((int)std::floor((v + 1.0) * 0.5 * n), 0, n - 1);
    out = makeBox(seed, face, i, j, n, planetRadiusM, surfaceElevM, user);
}

bool caveBoxAt(uint32_t seed, const glm::dvec3& dir, double planetRadiusM, CaveBox& out,
               float (*surfaceElevM)(const glm::dvec3&, void*), void* user) {
    if (planetRadiusM <= 0.0 || g_caveAutoProb <= 0.0f) return false;
    const int n = cellsPerFace();
    int face; double u, v;
    dirToFaceUV(glm::normalize(dir), face, u, v);
    const int i = std::clamp((int)std::floor((u + 1.0) * 0.5 * n), 0, n - 1);
    const int j = std::clamp((int)std::floor((v + 1.0) * 0.5 * n), 0, n - 1);
    const uint32_t key = cellKey(face, i, j, n);
    if (hash01(key, seed ^ 0xcafe1234u) > g_caveAutoProb) return false;   // esta celda no tiene cueva
    out = makeBox(seed, face, i, j, n, planetRadiusM, surfaceElevM, user);
    return true;
}

int caveBoxesNear(uint32_t seed, const glm::dvec3& nearDir, double searchRadiusM,
                  double planetRadiusM, CaveBox* out, int maxOut,
                  float (*surfaceElevM)(const glm::dvec3&, void*), void* user) {
    if (!out || maxOut <= 0 || planetRadiusM <= 0.0 || g_caveAutoProb <= 0.0f) return 0;
    const glm::dvec3 obs = glm::normalize(nearDir);
    const double searchRad = searchRadiusM / planetRadiusM;   // radio angular
    const int n = cellsPerFace();
    const int k = std::clamp((int)std::ceil(searchRad / kCaveCellRad) + 1, 1, 24);

    // Barrido en el plano tangente del observador (como `activeVortices`): se prueba una rejilla de
    // direcciones alrededor y se anotan las celdas distintas que caen dentro.
    const glm::dvec3 ref = (std::fabs(obs.y) < 0.99) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const glm::dvec3 e1 = glm::normalize(glm::cross(ref, obs));
    const glm::dvec3 e2 = glm::cross(obs, e1);

    int count = 0;
    std::vector<uint32_t> seen;
    seen.reserve((size_t)(2 * k + 1) * (2 * k + 1));
    for (int di = -k; di <= k && count < maxOut; ++di)
        for (int dj = -k; dj <= k && count < maxOut; ++dj) {
            const glm::dvec3 probe = glm::normalize(
                obs + e1 * ((double)di * kCaveCellRad) + e2 * ((double)dj * kCaveCellRad));
            int face; double u, v;
            dirToFaceUV(probe, face, u, v);
            const int ci = std::clamp((int)std::floor((u + 1.0) * 0.5 * n), 0, n - 1);
            const int cj = std::clamp((int)std::floor((v + 1.0) * 0.5 * n), 0, n - 1);
            const uint32_t key = cellKey(face, ci, cj, n);
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
            seen.push_back(key);
            if (hash01(key, seed ^ 0xcafe1234u) > g_caveAutoProb) continue;
            CaveBox b = makeBox(seed, face, ci, cj, n, planetRadiusM, surfaceElevM, user);
            if (std::acos(std::clamp(glm::dot(b.centerDir, obs), -1.0, 1.0)) > searchRad) continue;
            out[count++] = b;
        }
    return count;
}

/// Dónde cae el pozo de entrada, en fracción de la caja [0,1]². Determinista y COMPARTIDO por el
/// horneado de los mapas y el del volumen: si cada uno lo calculara por su cuenta, el día que
/// cambie uno la boca dejaría de coincidir con la galería.
static void entrancePoint(const CaveBox& box, float& ex, float& ey, float& er) {
    ex = box.mouth.x; ey = box.mouth.y; er = box.mouth.z;
}

glm::dvec3 caveEntranceDir(const CaveBox& box) {
    float ex, ey, er;
    entrancePoint(box, ex, ey, er);
    return glm::normalize(box.toWorld(glm::vec3((ex - 0.5f) * 2.0f * box.radiusM,
                                                (ey - 0.5f) * 2.0f * box.radiusM, 0.0f)));
}

float caveEntranceRadiusM(const CaveBox& box) {
    float ex, ey, er;
    entrancePoint(box, ex, ey, er);
    return er * 2.0f * box.radiusM;
}

// ── Horneado de los dos mapas ───────────────────────────────────────────────────────────────────

void caveBakeMaps(const CaveBox& box, int res, CaveMap& planta, CaveMap& perfil) {
    res = std::max(8, res);
    planta.w = planta.h = res; planta.v.assign((size_t)res * res, 0.0f);
    perfil.w = res; perfil.h = res / 2; perfil.v.assign((size_t)perfil.w * perfil.h, 0.0f);

    const uint32_t s = box.seed ^ (box.cell * 2654435761u);
    // Escala de los corredores: ~40-90 m de paso entre galerías. Con la caja en [0,1] eso son unas
    // pocas vueltas de ruido, y es lo que decide si la cueva es una sala o un laberinto.
    const float freqTop = 2.5f + 3.5f * hash01(box.cell, s ^ 0x1111u);
    const float freqSide = 2.0f + 2.0f * hash01(box.cell, s ^ 0x2222u);

    // PLANTA (M_top): la huella vista desde arriba. Crestas del ridged = corredores; el fBm suave
    // las curva (warp) para que no salgan rectas de libro de texto.
    for (int y = 0; y < res; ++y)
        for (int x = 0; x < res; ++x) {
            const float u = (float)x / (float)(res - 1), t = (float)y / (float)(res - 1);
            const float wx = 0.35f * fbm2(u * 3.0f + 11.3f, t * 3.0f, s ^ 0x3333u, 3);
            const float wy = 0.35f * fbm2(u * 3.0f, t * 3.0f + 7.7f, s ^ 0x4444u, 3);
            float m = ridged2((u + wx) * freqTop, (t + wy) * freqTop, s ^ 0x5555u, 4);
            // Se apaga hacia el borde de la caja: una cueva no puede salirse de su caja, o el
            // vecino la cortaría en seco (y el vecino no sabe que existe).
            const float dx = (u - 0.5f) * 2.0f, dy = (t - 0.5f) * 2.0f;
            const float rr = std::sqrt(dx * dx + dy * dy);
            m *= 1.0f - std::clamp((rr - 0.72f) / 0.28f, 0.0f, 1.0f);
            planta.v[(size_t)y * res + x] = std::clamp(m, 0.0f, 1.0f);
        }

    // PERFIL (M_side): qué profundidades tienen hueco. Eje x = un eje tangente, eje y = profundidad
    // (0 arriba). Dos cosas que NO son ruido y que se imponen aquí:
    //   · un TECHO: los primeros metros bajo la superficie son roca, salvo donde se abre la boca
    //     (eso lo decide el pozo de abajo). Sin esto el terreno queda hueco por todas partes.
    //   · una PLANTA de nivel: el ridged en vertical da estratos, que es como se ven las cuevas
    //     reales (galerías por niveles, no una esponja isótropa).
    for (int y = 0; y < perfil.h; ++y)
        for (int x = 0; x < perfil.w; ++x) {
            const float u = (float)x / (float)(perfil.w - 1);
            const float t = (float)y / (float)(perfil.h - 1);          // 0 = superficie
            float m = ridged2(u * freqSide, t * freqSide * 2.2f, s ^ 0x6666u, 4);
            m *= std::clamp((t - 0.06f) / 0.12f, 0.0f, 1.0f);          // techo de roca
            m *= 1.0f - std::clamp((t - 0.85f) / 0.15f, 0.0f, 1.0f);   // suelo de la caja
            perfil.v[(size_t)y * perfil.w + x] = std::clamp(m, 0.0f, 1.0f);
        }

    // ⚠️ EL POZO DE ENTRADA NO SE IMPRIME AQUÍ, Y ESO ES UN ARREGLO. Estaba puesto en los dos
    // mapas (un disco en planta, una columna en perfil) y salía una BARRA de lado a lado del parche
    // en vez de una boca: el perfil sólo depende de (x, profundidad), así que su columna se EXTRUYE
    // a lo largo del otro eje tangente — la limitación de fondo de describir un volumen como
    // producto de dos proyecciones. Se abre en el volumen, como un trazo más (ver `caveBakeVolume`),
    // que además es exactamente lo que hace el jugador cuando pica: un solo mecanismo.
}

// ── Horneado del volumen + conectividad ─────────────────────────────────────────────────────────

void caveBakeVolume(const CaveBox& box, const CaveMap& planta, const CaveMap& perfil,
                    int n, CaveVolume& out, glm::ivec3* outStats, int minVoxels) {
    n = std::max(8, n);
    out.n = n;
    out.d.assign((size_t)n * n * n, 1.0f);
    if (planta.empty() || perfil.empty()) { if (outStats) *outStats = glm::ivec3(0); return; }

    // τ: el umbral de porosidad. Por celda, para que unas cuevas sean amplias y otras estrechas.
    const uint32_t s = box.seed ^ (box.cell * 2654435761u);
    const float tau = 0.34f + 0.10f * hash01(box.cell, s ^ 0xaaaau);

    // 1. Rasterizar el aire: aire = planta·perfil > τ.
    std::vector<uint8_t> air((size_t)n * n * n, 0);
    auto idx = [n](int x, int y, int z) { return (size_t)z * n * n + (size_t)y * n + x; };
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const float u = (float)x / (float)(n - 1), v = (float)y / (float)(n - 1);
                const float t = (float)z / (float)(n - 1);
                const float a = planta.at(u, v) * perfil.at(u, t);
                air[idx(x, y, z)] = (a > tau) ? 1 : 0;
            }

    // 1.b EL POZO DE ENTRADA, como TRAZO sobre la rejilla y ANTES de la conectividad: así el
    //     flood-fill lo ve, lo une a la galería principal y la boca queda dentro de la única pieza
    //     de aire. (Si se abriera después, el pozo podría quedar como una bolsa suelta.)
    {
        float ex, ey, er;
        entrancePoint(box, ex, ey, er);
        const float cx = ex * (float)(n - 1), cy = ey * (float)(n - 1);
        const float rVox = std::max(er * (float)(n - 1), 1.5f);
        // Baja hasta encontrar aire (la primera galería) y un poco más, para que enganche de verdad.
        int zFondo = n - 1;
        for (int z = 0; z < n; ++z) {
            const int ix = std::clamp((int)std::lround(cx), 0, n - 1);
            const int iy = std::clamp((int)std::lround(cy), 0, n - 1);
            bool hayAire = false;
            for (int dy = -2; dy <= 2 && !hayAire; ++dy)
                for (int dx = -2; dx <= 2 && !hayAire; ++dx) {
                    const int jx = std::clamp(ix + dx, 0, n - 1), jy = std::clamp(iy + dy, 0, n - 1);
                    hayAire = air[idx(jx, jy, z)] != 0;
                }
            if (hayAire) { zFondo = std::min(n - 1, z + 2); break; }
        }
        for (int z = 0; z <= zFondo; ++z)
            for (int y = std::max(0, (int)(cy - rVox)); y <= std::min(n - 1, (int)(cy + rVox)); ++y)
                for (int x = std::max(0, (int)(cx - rVox)); x <= std::min(n - 1, (int)(cx + rVox)); ++x) {
                    const float dx = (float)x - cx, dy = (float)y - cy;
                    if (dx * dx + dy * dy <= rVox * rVox) air[idx(x, y, z)] = 1;
                }
    }

    // 2. Componentes conexas (26 vecinos) por flood-fill iterativo.
    std::vector<int> comp((size_t)n * n * n, -1);
    std::vector<std::vector<size_t>> comps;
    std::vector<size_t> stack;
    for (size_t start = 0; start < air.size(); ++start) {
        if (!air[start] || comp[start] >= 0) continue;
        const int id = (int)comps.size();
        comps.emplace_back();
        stack.clear(); stack.push_back(start); comp[start] = id;
        while (!stack.empty()) {
            const size_t c = stack.back(); stack.pop_back();
            comps[id].push_back(c);
            const int z = (int)(c / ((size_t)n * n));
            const int y = (int)((c / n) % n);
            const int x = (int)(c % n);
            for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dx && !dy && !dz) continue;
                        const int nx = x + dx, ny = y + dy, nz = z + dz;
                        if (nx < 0 || ny < 0 || nz < 0 || nx >= n || ny >= n || nz >= n) continue;
                        const size_t k = idx(nx, ny, nz);
                        if (!air[k] || comp[k] >= 0) continue;
                        comp[k] = id; stack.push_back(k);
                    }
        }
    }
    const int compsBefore = (int)comps.size();

    // 3. La mayor se queda; a cada bolsa que merezca la pena se le talla un túnel hasta ella, y las
    //    que no lo merecen se rellenan de roca. ⚠️ ESTO ES EL PUNTO DEL POST-PROCESO: el AND de dos
    //    mapas conexos NO es conexo (lo dice §A.3 del plan y lo confirma `compsBefore`), así que
    //    confiar en el AND deja salas a las que no se puede llegar por ningún sitio.
    int air1 = 0;
    if (!comps.empty()) {
        size_t big = 0;
        for (size_t i = 1; i < comps.size(); ++i)
            if (comps[i].size() > comps[big].size()) big = i;

        auto centroid = [&](const std::vector<size_t>& c) {
            glm::vec3 m(0.0f);
            for (size_t k : c) m += glm::vec3((float)(k % n), (float)((k / n) % n), (float)(k / ((size_t)n * n)));
            return m / (float)c.size();
        };
        const glm::vec3 cBig = centroid(comps[big]);

        for (size_t i = 0; i < comps.size(); ++i) {
            if (i == big) continue;
            if ((int)comps[i].size() < minVoxels) {           // hueco-punto: a roca
                for (size_t k : comps[i]) air[k] = 0;
                continue;
            }
            // Túnel del vóxel de la bolsa más cercano a la componente mayor, hasta el vóxel de la
            // mayor más cercano a la bolsa: recto, y con radio de un par de vóxeles (gateable).
            const glm::vec3 cSmall = centroid(comps[i]);
            auto nearestIn = [&](const std::vector<size_t>& c, const glm::vec3& to) {
                size_t best = c[0]; float bd = 1e30f;
                for (size_t k : c) {
                    const glm::vec3 p((float)(k % n), (float)((k / n) % n), (float)(k / ((size_t)n * n)));
                    const float d = glm::dot(p - to, p - to);
                    if (d < bd) { bd = d; best = k; }
                }
                return best;
            };
            const size_t ka = nearestIn(comps[i], cBig), kb = nearestIn(comps[big], cSmall);
            const glm::vec3 A((float)(ka % n), (float)((ka / n) % n), (float)(ka / ((size_t)n * n)));
            const glm::vec3 B((float)(kb % n), (float)((kb / n) % n), (float)(kb / ((size_t)n * n)));
            const float len = glm::length(B - A);
            const int steps = std::max(2, (int)std::ceil(len * 2.0f));
            const float rad = 1.6f;
            for (int t = 0; t <= steps; ++t) {
                const glm::vec3 p = A + (B - A) * ((float)t / (float)steps);
                const int r = (int)std::ceil(rad);
                for (int dz = -r; dz <= r; ++dz)
                    for (int dy = -r; dy <= r; ++dy)
                        for (int dx = -r; dx <= r; ++dx) {
                            const int x = (int)std::lround(p.x) + dx;
                            const int y = (int)std::lround(p.y) + dy;
                            const int z = (int)std::lround(p.z) + dz;
                            if (x < 0 || y < 0 || z < 0 || x >= n || y >= n || z >= n) continue;
                            if ((float)(dx * dx + dy * dy + dz * dz) > rad * rad) continue;
                            air[idx(x, y, z)] = 1;
                        }
            }
        }
    }
    for (uint8_t a : air) air1 += a;

    // 4. Recuento DESPUÉS (la contraprueba del paso 3: si sigue habiendo más de una componente, el
    //    tallado no hizo su trabajo y el test tiene que fallar).
    int compsAfter = 0;
    {
        std::vector<uint8_t> vis((size_t)n * n * n, 0);
        for (size_t start = 0; start < air.size(); ++start) {
            if (!air[start] || vis[start]) continue;
            ++compsAfter;
            stack.clear(); stack.push_back(start); vis[start] = 1;
            while (!stack.empty()) {
                const size_t c = stack.back(); stack.pop_back();
                const int z = (int)(c / ((size_t)n * n)), y = (int)((c / n) % n), x = (int)(c % n);
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = x + dx, ny = y + dy, nz = z + dz;
                            if (nx < 0 || ny < 0 || nz < 0 || nx >= n || ny >= n || nz >= n) continue;
                            const size_t k = idx(nx, ny, nz);
                            if (!air[k] || vis[k]) continue;
                            vis[k] = 1; stack.push_back(k);
                        }
            }
        }
    }
    if (outStats) *outStats = glm::ivec3(compsBefore, compsAfter, air1);

    // 5. El campo firmado: distancia (en vóxeles) al cambio de medio, por barrido de chanfle 3-4-5
    //    en dos pasadas. Un binario no sirve como campo: la malla saldría en escalones de vóxel y
    //    el gradiente —que es lo que empuja al jugador fuera de la pared— sería cero o infinito.
    // El vóxel NO es cúbico (la caja es ancha y poco profunda): el chanfle cuenta en vóxeles, así
    // que se pasa a metros con el lado MENOR — así el campo nunca dice que la pared está más lejos
    // de lo que está, que es el error que atraviesa paredes.
    const float voxM = std::min(2.0f * box.radiusM / (float)n, box.depthM / (float)n);
    // ⚠️ LA SIEMBRA VA CRUZADA, y esto ya me mordió: `din` es "cuánto aire hay hasta la pared" y
    // por tanto arranca en CERO SOBRE LA ROCA y se propaga hacia dentro del aire; `dout` al revés.
    // Sembrando cada uno sobre su propio medio, todo vóxel de aire salía a distancia 0 — o sea el
    // campo era idénticamente cero dentro de la cueva: sin gradiente, sin pared y sin boca (el banco
    // lo cazó: "0 de 4096 puntos de superficie con aire").
    std::vector<float> din((size_t)n * n * n, 1e9f), dout((size_t)n * n * n, 1e9f);
    for (size_t k = 0; k < air.size(); ++k) { (air[k] ? dout : din)[k] = 0.0f; }
    auto chamfer = [&](std::vector<float>& f) {
        auto rel = [&](int x, int y, int z, float w, float& best) {
            if (x < 0 || y < 0 || z < 0 || x >= n || y >= n || z >= n) return;
            best = std::min(best, f[idx(x, y, z)] + w);
        };
        for (int z = 0; z < n; ++z) for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
            float b = f[idx(x, y, z)];
            for (int dz = -1; dz <= 0; ++dz) for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                if (dz == 0 && (dy > 0 || (dy == 0 && dx >= 0))) continue;
                const int m = std::abs(dx) + std::abs(dy) + std::abs(dz);
                rel(x + dx, y + dy, z + dz, (m == 1 ? 3.0f : (m == 2 ? 4.0f : 5.0f)) / 3.0f, b);
            }
            f[idx(x, y, z)] = b;
        }
        for (int z = n - 1; z >= 0; --z) for (int y = n - 1; y >= 0; --y) for (int x = n - 1; x >= 0; --x) {
            float b = f[idx(x, y, z)];
            for (int dz = 0; dz <= 1; ++dz) for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                if (dz == 0 && (dy < 0 || (dy == 0 && dx <= 0))) continue;
                const int m = std::abs(dx) + std::abs(dy) + std::abs(dz);
                rel(x + dx, y + dy, z + dz, (m == 1 ? 3.0f : (m == 2 ? 4.0f : 5.0f)) / 3.0f, b);
            }
            f[idx(x, y, z)] = b;
        }
    };
    chamfer(din); chamfer(dout);
    for (size_t k = 0; k < out.d.size(); ++k)
        out.d[k] = (air[k] ? -din[k] : dout[k]) * voxM;   // <0 aire, >0 roca, en metros
}

// ── Cache en disco ──────────────────────────────────────────────────────────────────────────────

std::string caveCacheKey(const CaveBox& box, int mapRes, int volRes) {
    uint64_t h = 1469598103934665603ull;
    const char* version = "cave-bake-v2";   // v2: surfaceRM en double
    mix(h, version, std::strlen(version));
    mix(h, &box.seed, sizeof(box.seed));
    mix(h, &box.cell, sizeof(box.cell));
    mix(h, &box.radiusM, sizeof(box.radiusM));
    mix(h, &box.depthM, sizeof(box.depthM));
    mix(h, &box.surfaceRM, sizeof(box.surfaceRM));
    mix(h, &mapRes, sizeof(mapRes));
    mix(h, &volRes, sizeof(volRes));
    char hex[20];
    std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)h);
    return std::string(hex);
}

namespace {
/// El volumen va a PNG como ATLAS de rodajas: `n` rodajas de n×n en una cuadrícula. Un PNG es lo
/// que ya sabe leer y escribir el motor, y además se puede ABRIR y mirar, que es medio motivo de
/// tener la cueva en un fichero. Se guarda el campo remapeado a [0,1] con 0,5 = la pared.
// ⚠️ EL FICHERO SATURA A ±24 m, A PROPÓSITO. El campo llega a valer cientos de metros en el corazón
// de la roca, pero ahí sólo importa el SIGNO: lo que se usa de verdad —la malla, la normal de la
// pared, el empuje de la colisión— vive en los primeros metros. Guardar el rango completo en un byte
// daría 1,6 m de paso junto a la pared (el jugador se hundiría metro y medio); saturando, el paso es
// de 19 cm. Si algún día hace falta distancia lejos de la pared, es un PNG de 16 bits, no un rango
// más ancho.
constexpr float kVolRangeM = 24.0f;

void volToAtlas(const CaveVolume& v, int& w, int& h, std::vector<float>& px) {
    const int n = v.n;
    const int cols = (int)std::ceil(std::sqrt((double)n));
    const int rows = (cols > 0) ? (n + cols - 1) / cols : 1;
    w = cols * n; h = rows * n;
    px.assign((size_t)w * h, 1.0f);
    for (int z = 0; z < n; ++z) {
        const int cx = (z % cols) * n, cy = (z / cols) * n;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const float d = v.d[(size_t)z * n * n + (size_t)y * n + x];
                px[(size_t)(cy + y) * w + (cx + x)] =
                    std::clamp(d / (2.0f * kVolRangeM) + 0.5f, 0.0f, 1.0f);
            }
    }
}

bool atlasToVol(int w, int h, const std::vector<float>& px, int n, CaveVolume& v) {
    const int cols = (int)std::ceil(std::sqrt((double)n));
    const int rows = (cols > 0) ? (n + cols - 1) / cols : 1;
    if (w != cols * n || h != rows * n) return false;
    v.n = n; v.d.assign((size_t)n * n * n, 1.0f);
    for (int z = 0; z < n; ++z) {
        const int cx = (z % cols) * n, cy = (z / cols) * n;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                v.d[(size_t)z * n * n + (size_t)y * n + x] =
                    (px[(size_t)(cy + y) * w + (cx + x)] - 0.5f) * (2.0f * kVolRangeM);
    }
    return true;
}
} // namespace

bool caveLoadOrBake(const CaveBox& box, CaveSystem& out, const std::string& dir,
                    int mapRes, int volRes) {
    out.box = box;
    const std::string base = bakesDir(dir) + "cave_" + caveCacheKey(box, mapRes, volRes);

    int w = 0, h = 0;
    std::vector<float> px;
    const bool haveP = loadPNGGray(base + "_planta.png", w, h, px);
    if (haveP) { out.planta.w = w; out.planta.h = h; out.planta.v = px; }
    if (loadPNGGray(base + "_perfil.png", w, h, px)) { out.perfil.w = w; out.perfil.h = h; out.perfil.v = px; }
    if (out.planta.empty() || out.perfil.empty()) {
        caveBakeMaps(box, mapRes, out.planta, out.perfil);
        savePNGGray(base + "_planta.png", out.planta.w, out.planta.h, out.planta.v);
        savePNGGray(base + "_perfil.png", out.perfil.w, out.perfil.h, out.perfil.v);
    }

    // El volumen es DERIVADO: si falta o no casa, se rehornea de los mapas. Así, si alguien pinta
    // la planta a mano, basta con borrar el `_vol.png` (o cambiar la clave) para que se rehaga.
    if (loadPNGGray(base + "_vol.png", w, h, px) && atlasToVol(w, h, px, volRes, out.vol)) {
        out.connectStats = glm::ivec3(0, 0, 0);   // no se recuenta al cargar: lo garantizó el horneado
        if (loadPNGGray(base + "_boca.png", w, h, px)) { out.boca.w = w; out.boca.h = h; out.boca.v = px; }
        else {   // derivado como el volumen: si falta, se rehace
            caveBakeEntrance(box, out.vol, mapRes, out.boca);
            savePNGGray(base + "_boca.png", out.boca.w, out.boca.h, out.boca.v);
        }
        return true;
    }
    const auto t0 = std::chrono::high_resolution_clock::now();
    caveBakeVolume(box, out.planta, out.perfil, volRes, out.vol, &out.connectStats);
    caveBakeEntrance(box, out.vol, mapRes, out.boca);
    savePNGGray(base + "_boca.png", out.boca.w, out.boca.h, out.boca.v);
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::high_resolution_clock::now() - t0).count();
    int aw = 0, ah = 0; std::vector<float> apx;
    volToAtlas(out.vol, aw, ah, apx);
    savePNGGray(base + "_vol.png", aw, ah, apx);
    HARUKA_LOGI("Cuevas", "celda %u horneada en %.0f ms: %d componentes -> %d · %d voxeles de aire",
                box.cell, ms, out.connectStats.x, out.connectStats.y, out.connectStats.z);
    return !out.vol.empty();
}

// ── Cuevas de la escena (sin semilla) ───────────────────────────────────────────────────────────

uint32_t caveIdFromName(const std::string& name) {
    uint64_t h = 1469598103934665603ull;
    mix(h, name.data(), name.size());
    // Bit alto puesto: nunca choca con una clave de celda de borrador (6·n² < 2^31).
    return (uint32_t)(h ^ (h >> 32)) | 0x80000000u;
}

std::string caveResolveAssetPath(const std::string& path) {
    if (path.empty()) return path;
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return path;
    const std::string& root = AssetPaths::projectRoot();
    return root.empty() ? path : root + path;
}

CaveBox caveBoxFromDef(const CaveDef& def, double planetRadiusM,
                       float (*surfaceElevM)(const glm::dvec3&, void*), void* user) {
    CaveBox b;
    b.seed = def.draftSeed;
    b.cell = caveIdFromName(def.name);
    b.centerDir = glm::normalize(def.centerDir);
    const glm::dvec3 ref = (std::fabs(b.centerDir.y) < 0.99) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    b.tangentU = glm::normalize(glm::cross(ref, b.centerDir));
    b.tangentV = glm::cross(b.centerDir, b.tangentU);
    b.radiusM = std::max(def.radiusM, 20.0f);
    b.depthM  = std::max(def.depthM, 10.0f);
    b.mouth   = def.mouth;
    const float elev = surfaceElevM ? surfaceElevM(b.centerDir, user) : 0.0f;
    b.surfaceRM = planetRadiusM + (double)elev;
    return b;
}

namespace {
uint64_t mapHash(const CaveMap& m) {
    uint64_t h = 1469598103934665603ull;
    mix(h, &m.w, sizeof(m.w)); mix(h, &m.h, sizeof(m.h));
    if (!m.v.empty()) mix(h, m.v.data(), m.v.size() * sizeof(float));
    return h;
}
} // namespace

bool caveLoad(const CaveDef& def, CaveSystem& out, double planetRadiusM, const std::string& dir,
              int mapRes, int volRes, float (*surfaceElevM)(const glm::dvec3&, void*), void* user) {
    return caveLoadBox(def, caveBoxFromDef(def, planetRadiusM, surfaceElevM, user), out, dir, mapRes, volRes);
}

bool caveLoadBox(const CaveDef& def, const CaveBox& box, CaveSystem& out, const std::string& dir,
                 int mapRes, int volRes) {
    out = CaveSystem{};
    out.box = box;
    // Los mapas: de sus rutas. Si faltan, BORRADOR con la semilla, guardado en esas rutas: a partir
    // de ahí son autoría y el generador no vuelve a tocarlos.
    int w = 0, h = 0; std::vector<float> px;
    const std::string pPlanta = caveResolveAssetPath(def.planta), pPerfil = caveResolveAssetPath(def.perfil);
    if (!pPlanta.empty() && loadPNGGray(pPlanta, w, h, px)) { out.planta.w = w; out.planta.h = h; out.planta.v = px; }
    if (!pPerfil.empty() && loadPNGGray(pPerfil, w, h, px)) { out.perfil.w = w; out.perfil.h = h; out.perfil.v = px; }
    if (out.planta.empty() || out.perfil.empty()) {
        caveBakeMaps(out.box, mapRes, out.planta, out.perfil);
        if (!pPlanta.empty()) savePNGGray(pPlanta, out.planta.w, out.planta.h, out.planta.v);
        if (!pPerfil.empty()) savePNGGray(pPerfil, out.perfil.w, out.perfil.h, out.perfil.v);
        HARUKA_LOGI("Cuevas", "'%s': sin mapas -> borrador (semilla %u)%s", def.name.c_str(), def.draftSeed,
                    pPlanta.empty() ? " (no se guarda: sin ruta)" : ", guardado como asset");
    }
    return caveDeriveFromMaps(out, dir, volRes);
}

bool caveDeriveFromMaps(CaveSystem& out, const std::string& dir, int volRes) {
    if (out.planta.empty() || out.perfil.empty()) return false;
    // Clave = la caja + los TEXELS de los mapas. Nada de semilla: dos mundos con los mismos
    // ficheros comparten el volumen derivado.
    uint64_t hh = 1469598103934665603ull;
    const char* version = "cave-def-v1";
    mix(hh, version, std::strlen(version));
    mix(hh, &out.box.radiusM, sizeof(out.box.radiusM));
    mix(hh, &out.box.depthM, sizeof(out.box.depthM));
    mix(hh, &out.box.surfaceRM, sizeof(out.box.surfaceRM));
    mix(hh, &out.box.mouth, sizeof(out.box.mouth));
    mix(hh, &out.box.seed, sizeof(out.box.seed));   // el umbral τ de `caveBakeVolume` sale de aquí
    mix(hh, &volRes, sizeof(volRes));
    const uint64_t hp = mapHash(out.planta), hs = mapHash(out.perfil);
    mix(hh, &hp, sizeof(hp)); mix(hh, &hs, sizeof(hs));
    const std::string base = bakesDir(dir) + "cave_" + hex16(hh);
    int w = 0, h = 0; std::vector<float> px;
    if (loadPNGGray(base + "_vol.png", w, h, px) && atlasToVol(w, h, px, volRes, out.vol)) {
        out.connectStats = glm::ivec3(0, 0, 0);
        if (loadPNGGray(base + "_boca.png", w, h, px)) { out.boca.w = w; out.boca.h = h; out.boca.v = px; }
        else { caveBakeEntrance(out.box, out.vol, out.planta.w, out.boca); savePNGGray(base + "_boca.png", out.boca.w, out.boca.h, out.boca.v); }
        return true;
    }
    const auto t0 = std::chrono::high_resolution_clock::now();
    caveBakeVolume(out.box, out.planta, out.perfil, volRes, out.vol, &out.connectStats);
    caveBakeEntrance(out.box, out.vol, out.planta.w, out.boca);
    savePNGGray(base + "_boca.png", out.boca.w, out.boca.h, out.boca.v);
    int aw = 0, ah = 0; std::vector<float> apx;
    volToAtlas(out.vol, aw, ah, apx);
    savePNGGray(base + "_vol.png", aw, ah, apx);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
    HARUKA_LOGI("Cuevas", "cueva %u derivada de sus mapas en %.0f ms: %d componentes -> %d · %d voxeles de aire",
                out.box.cell, ms, out.connectStats.x, out.connectStats.y, out.connectStats.z);
    return !out.vol.empty();
}

// ── Muestreo ────────────────────────────────────────────────────────────────────────────────────

float caveDensityLocal(const CaveSystem& sys, const glm::vec3& l);

float caveDensityAtDraped(const CaveSystem& sys, const glm::dvec3& p, double surfaceR) {
    if (sys.vol.empty()) return 1e9f;
    // Mismas cuentas que `toLocal`, con el techo en `surfaceR` en vez de en `surfaceRM`: la
    // tangencial sigue medida desde el centro de la caja; la profundidad, desde el suelo LOCAL.
    const double r = glm::length(p);
    const glm::dvec3 rel = p - sys.box.centerDir * sys.box.surfaceRM;
    const glm::vec3 l((float)glm::dot(rel, sys.box.tangentU), (float)glm::dot(rel, sys.box.tangentV),
                      (float)(surfaceR - r));
    return caveDensityLocal(sys, l);
}

float caveDensityAt(const CaveSystem& sys, const glm::dvec3& p) {
    if (sys.vol.empty()) return 1e9f;
    return caveDensityLocal(sys, sys.box.toLocal(p));
}

float caveDensityLocal(const CaveSystem& sys, const glm::vec3& l) {
    float u = (l.x / sys.box.radiusM) * 0.5f + 0.5f;
    float v = (l.y / sys.box.radiusM) * 0.5f + 0.5f;
    float t =  l.z / std::max(sys.box.depthM, 1e-3f);
    // ⚠️ TOLERANCIA DE UN VÓXEL EN EL BORDE, y no es cosmética: la profundidad se mide contra el
    // RADIO (`surfaceRM − |p|`), así que un punto del techo de la caja desplazado 400 m de lado cae
    // 1,3 cm POR ENCIMA del techo por pura curvatura — y `surfaceRM` es un float, que a 6,37e6 m
    // tiene medio metro de ulp. Sin esta holgura, el primer vóxel de aire del volumen muestreaba
    // "fuera de la caja" (el banco: campo 1e9 donde el volumen decía aire). Fuera de la holgura
    // sigue siendo roca: un sistema no tiene efecto a distancia.
    const float tol = 1.0f / (float)std::max(sys.vol.n - 1, 1);
    if (u < -tol || u > 1.0f + tol || v < -tol || v > 1.0f + tol || t < -tol || t > 1.0f + tol)
        return 1e9f;
    u = std::clamp(u, 0.0f, 1.0f); v = std::clamp(v, 0.0f, 1.0f); t = std::clamp(t, 0.0f, 1.0f);

    const int n = sys.vol.n;
    const float fx = u * (float)(n - 1), fy = v * (float)(n - 1), fz = t * (float)(n - 1);
    const int x0 = std::clamp((int)fx, 0, n - 1), y0 = std::clamp((int)fy, 0, n - 1), z0 = std::clamp((int)fz, 0, n - 1);
    const int x1 = std::min(x0 + 1, n - 1), y1 = std::min(y0 + 1, n - 1), z1 = std::min(z0 + 1, n - 1);
    const float ax = fx - (float)x0, ay = fy - (float)y0, az = fz - (float)z0;
    auto D = [&](int x, int y, int z) { return sys.vol.d[(size_t)z * n * n + (size_t)y * n + x]; };
    const float c00 = D(x0,y0,z0) + (D(x1,y0,z0) - D(x0,y0,z0)) * ax;
    const float c10 = D(x0,y1,z0) + (D(x1,y1,z0) - D(x0,y1,z0)) * ax;
    const float c01 = D(x0,y0,z1) + (D(x1,y0,z1) - D(x0,y0,z1)) * ax;
    const float c11 = D(x0,y1,z1) + (D(x1,y1,z1) - D(x0,y1,z1)) * ax;
    const float c0 = c00 + (c10 - c00) * ay, c1 = c01 + (c11 - c01) * ay;
    return c0 + (c1 - c0) * az;
}

glm::vec3 caveDensityGradient(const CaveSystem& sys, const glm::dvec3& p, float hM) {
    // ⚠️ EL CENTINELA NO PUEDE ENTRAR EN UNA RESTA. `caveDensityAt` devuelve 1e9 fuera de la caja
    // para decir "aquí no hay cueva"; restarlo da un gradiente de 2e8, y quien use eso para sacar al
    // jugador de una pared lo dispara al espacio. Medido en el banco junto al borde de la caja:
    // |grad| = 197 822 128 donde un campo de distancia tiene que valer ~1. La muestra que cae fuera
    // se sustituye por la del centro (diferencia de un solo lado), que es lo que dice la física: el
    // borde de la caja no es una pared, es donde el sistema deja de tener algo que opinar.
    const float c = caveDensityAt(sys, p);
    const bool  cOut = (c > 1e8f);
    auto d1 = [&](const glm::dvec3& e) {
        float a = caveDensityAt(sys, p + e), b = caveDensityAt(sys, p - e);
        if (a > 1e8f) a = cOut ? 0.0f : c;
        if (b > 1e8f) b = cOut ? 0.0f : c;
        return (a - b) / (2.0f * hM);
    };
    return glm::vec3(d1(sys.box.tangentU * (double)hM),
                     d1(sys.box.tangentV * (double)hM),
                     d1(sys.box.centerDir * (double)hM));
}

// ── Malla: surface nets (dual) sobre el volumen ─────────────────────────────────────────────────

void harukaSurfaceNets(int n, const float* dd,
                       const std::function<glm::vec3(float, float, float)>& gridLocal,
                       CaveMesh& out, size_t maxTris, const uint8_t* own) {
    out.positions.clear(); out.normals.clear(); out.indices.clear();
    if (n < 2 || !dd) return;
    auto D = [&](int x, int y, int z) { return dd[(size_t)z * n * n + (size_t)y * n + x]; };

    // Un vértice por CELDA que cruce la superficie, colocado en la media de los cortes con signo de
    // sus 12 aristas (eso es lo que hace el dual: el CAMPO coloca el vértice, no una tabla de casos).
    std::vector<int> cellVert((size_t)(n - 1) * (n - 1) * (n - 1), -1);
    auto cellIdx = [&](int x, int y, int z) { return (size_t)z * (n - 1) * (n - 1) + (size_t)y * (n - 1) + x; };
    static const int kEdges[12][2][3] = {
        {{0,0,0},{1,0,0}}, {{0,1,0},{1,1,0}}, {{0,0,1},{1,0,1}}, {{0,1,1},{1,1,1}},
        {{0,0,0},{0,1,0}}, {{1,0,0},{1,1,0}}, {{0,0,1},{0,1,1}}, {{1,0,1},{1,1,1}},
        {{0,0,0},{0,0,1}}, {{1,0,0},{1,0,1}}, {{0,1,0},{0,1,1}}, {{1,1,0},{1,1,1}},
    };
    // Tamaño real de cada eje de la rejilla (puede ser anisótropa): la normal se corrige con él, o
    // se inclina hacia el eje corto.
    const glm::vec3 ex = gridLocal(1, 0, 0) - gridLocal(0, 0, 0);
    const glm::vec3 ey = gridLocal(0, 1, 0) - gridLocal(0, 0, 0);
    const glm::vec3 ez = gridLocal(0, 0, 1) - gridLocal(0, 0, 0);
    for (int z = 0; z < n - 1; ++z)
        for (int y = 0; y < n - 1; ++y)
            for (int x = 0; x < n - 1; ++x) {
                glm::vec3 acc(0.0f); int hits = 0;
                for (auto& e : kEdges) {
                    const int ax = x + e[0][0], ay = y + e[0][1], az = z + e[0][2];
                    const int bx = x + e[1][0], by = y + e[1][1], bz = z + e[1][2];
                    const float da = D(ax, ay, az), db = D(bx, by, bz);
                    if ((da < 0.0f) == (db < 0.0f)) continue;
                    const float t = da / (da - db);          // corte lineal del campo
                    acc += glm::mix(glm::vec3((float)ax, (float)ay, (float)az),
                                    glm::vec3((float)bx, (float)by, (float)bz), t);
                    ++hits;
                }
                if (!hits) continue;
                acc /= (float)hits;
                cellVert[cellIdx(x, y, z)] = (int)out.positions.size();
                out.positions.push_back(gridLocal(acc.x, acc.y, acc.z));
                // Normal del GRADIENTE del campo (hacia la roca), no de las caras: es la misma
                // normal que usará la colisión, así que lo que se ve y lo que se pisa coinciden.
                const int cx = std::clamp((int)std::lround(acc.x), 1, n - 2);
                const int cy = std::clamp((int)std::lround(acc.y), 1, n - 2);
                const int cz = std::clamp((int)std::lround(acc.z), 1, n - 2);
                const glm::vec3 g((D(cx + 1, cy, cz) - D(cx - 1, cy, cz)) / std::max(glm::length(ex), 1e-6f),
                                  (D(cx, cy + 1, cz) - D(cx, cy - 1, cz)) / std::max(glm::length(ey), 1e-6f),
                                  (D(cx, cy, cz + 1) - D(cx, cy, cz - 1)) / std::max(glm::length(ez), 1e-6f));
                const glm::vec3 gw = glm::normalize(ex) * g.x + glm::normalize(ey) * g.y + glm::normalize(ez) * g.z;
                const float gl = glm::length(gw);
                out.normals.push_back(gl > 1e-6f ? gw / gl : glm::normalize(ez));
            }

    // Un quad por arista de rejilla que cambia de signo, cosiendo las cuatro celdas que la comparten.
    auto emitQuad = [&](int a, int b, int c, int d, bool flip) {
        if (a < 0 || b < 0 || c < 0 || d < 0) return;
        if (out.indices.size() / 3 + 2 > maxTris) return;
        if (flip) { out.indices.insert(out.indices.end(), { (unsigned)a, (unsigned)b, (unsigned)c,
                                                            (unsigned)a, (unsigned)c, (unsigned)d }); }
        else      { out.indices.insert(out.indices.end(), { (unsigned)a, (unsigned)c, (unsigned)b,
                                                            (unsigned)a, (unsigned)d, (unsigned)c }); }
    };
    // ⚠️ LA PERTENENCIA SE MIRA EN EL EXTREMO DE AIRE, y sólo en él. Se pedía en los dos extremos, y
    // en el lado de ROCA el campo de la cueva satura a +24 m (lejos de toda galería) mientras la
    // profundidad bajo el suelo suele ser menor: "24 <= 10" es falso y el quad no salía. Resultado
    // medido en el juego: el pozo y las galerías a menos de 24 m del suelo NO tenían pared; las
    // hondas sí. Qué superficie es la que cruza lo decide el aire, no la roca de al lado.
    // `own` lleva DOS bits por nodo: bit 0 = el aire aquí es de la cueva (cueva <= terreno) ·
    // bit 1 = el nodo está BAJO el terreno. Un quad se cose si el extremo de aire es de la cueva
    // Y el extremo de roca está bajo tierra: sin lo segundo, una galería que corta una ladera
    // enseñaba sus paredes FUERA, flotando sobre el monte ("se ve por fuera").
    auto bits = [&](int x, int y, int z) -> unsigned { return own ? own[(size_t)z * n * n + (size_t)y * n + x] : 3u; };
    // Y el bit 2 (roca AÑADIDA: isla, parapeto) en el extremo de roca: esa superficie es del campo
    // aunque el aire de al lado sea el aire normal de encima del suelo.
    auto airOwned = [&](int ax, int ay, int az, int bx, int by, int bz) {
        const bool aAir = D(ax, ay, az) < 0.0f;
        const unsigned air = aAir ? bits(ax, ay, az) : bits(bx, by, bz);
        const unsigned rock = aAir ? bits(bx, by, bz) : bits(ax, ay, az);
        return ((air & 1u) != 0u || (rock & 4u) != 0u) && (rock & 2u) != 0u;
    };
    // ⚠️ EL BOBINADO MIRA AL AIRE. Jolt ignora las caras traseras de una malla para los rígidos
    // (`MeshShape` es de una cara para el narrow phase): con los triángulos mirando a la roca, un
    // objeto suelto atravesaba la cima de una isla y se paraba en la raíz POR DENTRO (medido en
    // `test_physics_island`: reposo a −50 m de la cima), y en una cueva atravesaría las paredes. El
    // personaje (`CharacterVirtual`, choca con las dos caras) no lo delataba. Las normales de los
    // vértices siguen apuntando a la roca (gradiente): el render las niega y la colisión empuja con
    // ellas; el bobinado es cosa aparte.
    for (int z = 1; z < n - 1; ++z)
        for (int y = 1; y < n - 1; ++y)
            for (int x = 1; x < n - 1; ++x) {
                const float d0 = D(x, y, z);
                if ((d0 < 0.0f) != (D(x + 1, y, z) < 0.0f) && airOwned(x, y, z, x + 1, y, z))
                    emitQuad((int)cellVert[cellIdx(x, y - 1, z - 1)], (int)cellVert[cellIdx(x, y, z - 1)],
                             (int)cellVert[cellIdx(x, y, z)],         (int)cellVert[cellIdx(x, y - 1, z)],
                             d0 >= 0.0f);
                if ((d0 < 0.0f) != (D(x, y + 1, z) < 0.0f) && airOwned(x, y, z, x, y + 1, z))
                    emitQuad((int)cellVert[cellIdx(x - 1, y, z - 1)], (int)cellVert[cellIdx(x, y, z - 1)],
                             (int)cellVert[cellIdx(x, y, z)],         (int)cellVert[cellIdx(x - 1, y, z)],
                             d0 < 0.0f);
                if ((d0 < 0.0f) != (D(x, y, z + 1) < 0.0f) && airOwned(x, y, z, x, y, z + 1))
                    emitQuad((int)cellVert[cellIdx(x - 1, y - 1, z)], (int)cellVert[cellIdx(x, y - 1, z)],
                             (int)cellVert[cellIdx(x, y, z)],         (int)cellVert[cellIdx(x - 1, y, z)],
                             d0 >= 0.0f);
            }
}

void caveBuildMesh(const CaveSystem& sys, CaveMesh& out, size_t maxTris) {
    out.positions.clear(); out.normals.clear(); out.indices.clear();
    if (sys.vol.empty()) return;
    const int n = sys.vol.n;
    const CaveBox& B = sys.box;
    out.origin = B.centerDir * B.surfaceRM;
    // El vóxel NO es cúbico: la caja es ancha y poco profunda, y la malla tiene que respetarlo.
    harukaSurfaceNets(n, sys.vol.d.data(), [&](float x, float y, float z) {
        return glm::vec3(((x / (float)(n - 1)) - 0.5f) * 2.0f * B.radiusM,
                         ((y / (float)(n - 1)) - 0.5f) * 2.0f * B.radiusM,
                          (z / (float)(n - 1)) * B.depthM);
    }, out, maxTris);
}

// ── Trazos ──────────────────────────────────────────────────────────────────────────────────────

int caveApplyStroke(CaveSystem& sys, const CaveStroke& st) {
    if (sys.vol.empty() || st.radiusM <= 0.0f) return 0;
    const int n = sys.vol.n;
    const CaveBox& B = sys.box;
    const float sx = 2.0f * B.radiusM / (float)(n - 1);     // metros por vóxel en x,y
    const float sz = B.depthM / (float)(n - 1);             // y en profundidad (no es cúbico)

    // Índice del centro y cuántos vóxeles abarca el radio en cada eje.
    const float fx = (st.centerLocal.x / B.radiusM * 0.5f + 0.5f) * (float)(n - 1);
    const float fy = (st.centerLocal.y / B.radiusM * 0.5f + 0.5f) * (float)(n - 1);
    const float fz = (st.centerLocal.z / std::max(B.depthM, 1e-3f)) * (float)(n - 1);
    const int kx = (int)std::ceil(st.radiusM / sx) + 1, kz = (int)std::ceil(st.radiusM / sz) + 1;

    int cambiados = 0;
    for (int z = std::max(0, (int)fz - kz); z <= std::min(n - 1, (int)fz + kz); ++z)
        for (int y = std::max(0, (int)fy - kx); y <= std::min(n - 1, (int)fy + kx); ++y)
            for (int x = std::max(0, (int)fx - kx); x <= std::min(n - 1, (int)fx + kx); ++x) {
                // Distancia REAL en metros (ejes anisótropos), no en vóxeles.
                const glm::vec3 p(((float)x - fx) * sx, ((float)y - fy) * sx, ((float)z - fz) * sz);
                const float dist = glm::length(p);
                float& d = sys.vol.d[(size_t)z * n * n + (size_t)y * n + x];
                const bool eraAire = d < 0.0f;
                // Unión / diferencia de campos de distancia con signo, que es lo que son estas dos
                // operaciones: abrir = unir aire (min), rellenar = unir roca (max).
                d = st.remove ? std::min(d, dist - st.radiusM)
                              : std::max(d, st.radiusM - dist);
                if ((d < 0.0f) != eraAire) ++cambiados;
            }
    return cambiados;
}

int caveApplyStrokeWorld(CaveSystem& sys, const glm::dvec3& worldPos, float radiusM, bool remove) {
    if (sys.vol.empty()) return -1;
    const glm::vec3 l = sys.box.toLocal(worldPos);
    // Se admite el trazo si su ESFERA toca la caja, no sólo si el centro cae dentro: picar desde
    // fuera hacia la pared es el caso normal.
    if (std::fabs(l.x) > sys.box.radiusM + radiusM || std::fabs(l.y) > sys.box.radiusM + radiusM ||
        l.z < -radiusM || l.z > sys.box.depthM + radiusM) return -1;
    CaveStroke st; st.centerLocal = l; st.radiusM = radiusM; st.remove = remove;
    return caveApplyStroke(sys, st);
}

bool caveSaveStrokes(const std::string& path, const std::vector<CaveStroke>& strokes) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    // v3: 13 columnas. Las líneas de 5 (v1, esferas) y de 7 (v2, formas) se siguen leyendo.
    std::fprintf(f, "# trazos (local x y z, radio m, 1=abrir 0=rellenar, forma 0=esfera 1=cilindro 2=caja 3=capsula, medio alto m, op 0=quitar 1=poner 2=allanar 3=subir 4=bajar 5=suavizar 6=esculpir, cantidad, dureza, extremo bx by bz, caida, patron, lados, paso del patron m)\n");
    for (const CaveStroke& s : strokes)
        std::fprintf(f, "%.3f %.3f %.3f %.3f %d %d %.3f %d %.3f %.3f %.3f %.3f %.3f %d %d %d %.3f\n", s.centerLocal.x, s.centerLocal.y,
                     s.centerLocal.z, s.radiusM, s.remove ? 1 : 0, (int)s.shape, s.halfM, (int)s.op, s.amountM, s.hardness,
                     s.endLocal.x, s.endLocal.y, s.endLocal.z, (int)s.falloff, (int)s.pattern, (int)s.sides, s.patternM);
    std::fclose(f);
    return true;
}

bool caveLoadStrokes(const std::string& path, std::vector<CaveStroke>& out) {
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        CaveStroke s; int rem = 1, shape = 0, op = -1, fall = 0, pat = 0, sides = 0; float half = 0.0f, amount = 4.0f, hard = 0.5f, patM = 8.0f; glm::vec3 e(0.0f);
        const int n = std::sscanf(line, "%f %f %f %f %d %d %f %d %f %f %f %f %f %d %d %d %f", &s.centerLocal.x, &s.centerLocal.y,
                                  &s.centerLocal.z, &s.radiusM, &rem, &shape, &half, &op, &amount, &hard, &e.x, &e.y, &e.z,
                                  &fall, &pat, &sides, &patM);
        if (n >= 5) {
            s.remove = (rem != 0);
            s.op = s.remove ? 0 : 1;
            if (n >= 7) { s.shape = (uint8_t)std::clamp(shape, 0, 3); s.halfM = half; }
            if (n >= 13) { s.op = (uint8_t)std::clamp(op, 0, 6); s.amountM = amount; s.hardness = hard; s.endLocal = e; }
            if (n >= 17) { s.falloff = (uint8_t)std::clamp(fall, 0, 6); s.pattern = (uint8_t)std::clamp(pat, 0, 5); s.sides = (uint8_t)std::clamp(sides, 0, 12); s.patternM = patM; }
            out.push_back(s);
        }
    }
    std::fclose(f);
    return true;
}

// ── El acoplamiento con el terreno ──────────────────────────────────────────────────────────────

void caveBakeEntrance(const CaveBox& box, const CaveVolume& vol, int res, CaveMap& out,
                      float metrosDeTecho) {
    res = std::max(8, res);
    out.w = out.h = res;
    out.v.assign((size_t)res * res, 0.0f);
    if (vol.empty()) return;
    const int n = vol.n;
    // Cuántas rodajas del volumen caben en `metrosDeTecho` de profundidad.
    const float porRodaja = box.depthM / (float)(n - 1);
    const int rodajas = std::clamp((int)std::ceil(metrosDeTecho / std::max(porRodaja, 1e-3f)), 1, n - 1);
    for (int y = 0; y < res; ++y)
        for (int x = 0; x < res; ++x) {
            // Se mira la columna de vóxeles bajo este punto: si en los primeros metros ya hay aire,
            // el techo no existe y esto es boca.
            // ⚠️ BILINEAL, NO EL VÓXEL MÁS CERCANO. El mapa es de 2,3 m por téxel y el volumen de 6 m
            // por vóxel: tomando el vóxel más próximo, téxeles vecinos leen el MISMO valor y el
            // recorte salta de 0 a 1 de golpe — el banco lo midió como "0,00 % de borde suave", que
            // en pantalla es un filo dentado del tamaño del vóxel, y el terreno es una malla
            // teselada que no puede seguir ese filo.
            const float u = (float)x / (float)(res - 1), v = (float)y / (float)(res - 1);
            const float fx = u * (float)(n - 1), fy = v * (float)(n - 1);
            const int x0 = std::clamp((int)fx, 0, n - 1), y0 = std::clamp((int)fy, 0, n - 1);
            const int x1 = std::min(x0 + 1, n - 1), y1 = std::min(y0 + 1, n - 1);
            const float ax = fx - (float)x0, ay = fy - (float)y0;
            float peor = 1e9f;
            for (int z = 0; z <= rodajas; ++z) {
                const size_t o = (size_t)z * n * n;
                const float a = vol.d[o + (size_t)y0 * n + x0], b = vol.d[o + (size_t)y0 * n + x1];
                const float c = vol.d[o + (size_t)y1 * n + x0], dd = vol.d[o + (size_t)y1 * n + x1];
                const float top = a + (b - a) * ax, bot = c + (dd - c) * ax;
                peor = std::min(peor, top + (bot - top) * ay);
            }
            // Borde suave: de 0 (pared) a 1 (aire franco) en 3 m. Un escalón duro deja el recorte
            // dentado al tamaño del téxel, y el terreno es una malla teselada que no lo puede seguir.
            out.v[(size_t)y * res + x] = std::clamp(-peor / 3.0f, 0.0f, 1.0f);
        }
}

float caveSurfaceCut(const CaveSystem& sys, const glm::dvec3& dir) {
    if (sys.boca.empty()) return 0.0f;
    // La dirección se lleva a la superficie de la caja y de ahí al plano tangente.
    const glm::vec3 l = sys.box.toLocal(glm::normalize(dir) * sys.box.surfaceRM);
    const float u = (l.x / sys.box.radiusM) * 0.5f + 0.5f;
    const float v = (l.y / sys.box.radiusM) * 0.5f + 0.5f;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return 0.0f;
    return sys.boca.at(u, v);
}

float caveEntranceMask(const CaveSystem& sys, const glm::dvec3& dir) {
    if (sys.vol.empty()) return 0.0f;
    // "Rompe la superficie" = hay aire justo bajo el techo de la caja. Se mide a un par de metros,
    // no a 0: a profundidad exactamente 0 el campo está en el borde del volumen y la trilineal
    // sujeta contra el borde.
    const glm::dvec3 p = glm::normalize(dir) * (sys.box.surfaceRM - 2.0);
    const float d = caveDensityAt(sys, p);
    if (d > 1e8f) return 0.0f;
    return std::clamp(-d / 4.0f, 0.0f, 1.0f);   // 0 en la pared, 1 a 4 m dentro del aire
}

} // namespace Haruka
