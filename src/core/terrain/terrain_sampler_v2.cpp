#include "terrain_sampler_v2.h"
#include "core/noise_generator.h"
#include "core/terrain/planet_geology.h"   // F2: tectónica de placas (PLAN_TERRENO_V3)
#include "core/terrain/planet_fields.h"    // F3: campos erosionados (LA FUENTE DE VERDAD)
#include "core/terrain/planet_meso.h"      // F3-MESO: el detalle erosionado que SÍ pisas
#include <mutex>
#include <memory>

#include <cmath>
#include <algorithm>
#include <vector>
#include <cstdint>

namespace Haruka {
namespace {

// ---------------------------------------------------------------------------
// ValD: número dual para autodiff forward. v = valor, g = ∇v respecto a `dir`.
// Permite escribir la función de altura como la escalar, arrastrando el gradiente
// exacto por cada operación no lineal (smoothstep, pow, abs, mix...).
// ---------------------------------------------------------------------------
struct ValD {
    float     v;
    glm::vec3 g;
};

inline ValD konst(float c)                 { return { c, glm::vec3(0.0f) }; }

// ===== F2 — GEOLOGÍA (tectónica de placas) ==================================================
// La elevación BASE deja de salir de un fBm y pasa a salir de las PLACAS: qué corteza hay aquí
// (continental → tierra, oceánica → mar) y qué le hace el límite más cercano (cordillera, fosa,
// arco volcánico, dorsal, rift). Es lo que convierte manchas de ruido en CORDILLERAS.
//
// Las placas se generan una vez por seed y se cachean: `sample()` es O(nº placas) ≈ 30 productos
// escalares, así que se puede llamar por vértice sin caché de campos. (La caché de heightfield
// llega en F3, cuando la EROSIÓN lo exija: un terreno erosionado ya no es función cerrada.)
const PlanetGeology& geologyFor(uint32_t seed) {
    static std::mutex mx;
    static std::vector<std::unique_ptr<PlanetGeology>> cache;   // pocas seeds vivas a la vez
    std::lock_guard<std::mutex> lk(mx);
    for (auto& g : cache) if (g->seed() == seed) return *g;
    auto g = std::make_unique<PlanetGeology>();
    g->generate(seed);
    cache.push_back(std::move(g));
    return *cache.back();
}

// --- F3: los CAMPOS EROSIONADOS son la fuente de verdad -------------------------------------
// La geología (F2) da la corteza y dónde se levanta montaña. Pero la ALTURA final ya no sale de esa
// fórmula: sale del campo EROSIONADO (valles tallados, cuencas, cauces). Un terreno erosionado no es
// una función cerrada — hay que precomputarlo y muestrearlo. Se genera una vez por seed (segundos)
// y se cachea; `sample` es un bilineal.
const PlanetFields& fieldsFor(uint32_t seed) {
    static std::mutex mx;
    static std::vector<std::unique_ptr<PlanetFields>> cache;
    std::lock_guard<std::mutex> lk(mx);
    for (auto& f : cache) if (f->seed() == seed) return *f;
    auto f = std::make_unique<PlanetFields>();
    f->generate(seed, geologyFor(seed), 256, 40);
    cache.push_back(std::move(f));
    return *cache.back();
}

// El MESO: teselas erosionadas a ~40 m/téxel alrededor de donde se pregunta. El macro (40 km/celda)
// tiene los valles pero no los PISAS; esto los baja a escala humana. Se genera bajo demanda y se
// cachea (LRU). Determinista → CPU y GPU obtienen la MISMA tesela aunque la generen por separado.
// Campo CON GRADIENTE (diferencias finitas): sin él, las normales de COLISIÓN ignorarían la
// pendiente de los valles tallados y caminarías por una ladera como si fuera llana.
// ⚠️ NO generamos teselas bajo demanda aquí: la física/spawn consultan direcciones por TODO el
// planeta (el spawn muestrea 4000) → generaríamos miles de teselas y el frame se caería. El meso se
// usa SOLO donde la tesela ya existe, y las pide el GENERADOR DE CHUNKS (o sea, alrededor del
// jugador — que es justo donde hay malla con la que colisionar). Fuera de ahí, macro.
ValD mesoElevD(const glm::vec3& dir, PlanetMeso& M) {
    const float v = M.elevKmIfResident(dir);
    const float h = 0.00002f;   // ~130 m en la Tierra: resuelve la ladera del valle, no el téxel
    glm::vec3 ref = (std::abs(dir.y) < 0.9f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    glm::vec3 t1  = glm::normalize(glm::cross(ref, dir));
    glm::vec3 t2  = glm::cross(dir, t1);
    const float a1 = M.elevKmIfResident(glm::normalize(dir + t1 * h));
    const float b1 = M.elevKmIfResident(glm::normalize(dir - t1 * h));
    const float a2 = M.elevKmIfResident(glm::normalize(dir + t2 * h));
    const float b2 = M.elevKmIfResident(glm::normalize(dir - t2 * h));
    const glm::vec3 g = t1 * ((a1 - b1) / (2.0f * h)) + t2 * ((a2 - b2) / (2.0f * h));
    return { v, g };
}

ValD fieldElevD(const glm::vec3& dir, const PlanetFields& F) {
    const float v = F.sample(dir).elevKm;
    const float h = 0.002f;
    glm::vec3 ref = (std::abs(dir.y) < 0.9f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    glm::vec3 t1  = glm::normalize(glm::cross(ref, dir));
    glm::vec3 t2  = glm::cross(dir, t1);
    const float a1 = F.sample(glm::normalize(dir + t1 * h)).elevKm;
    const float b1 = F.sample(glm::normalize(dir - t1 * h)).elevKm;
    const float a2 = F.sample(glm::normalize(dir + t2 * h)).elevKm;
    const float b2 = F.sample(glm::normalize(dir - t2 * h)).elevKm;
    const glm::vec3 g = t1 * ((a1 - b1) / (2.0f * h)) + t2 * ((a2 - b2) / (2.0f * h));
    return { v, g };
}

// Elevación geológica CON GRADIENTE. La geología no es analítica-derivable (hay un `min/max` por
// placa dentro), así que el gradiente va por DIFERENCIAS FINITAS en el plano tangente. Sin esto las
// normales de COLISIÓN ignorarían la pendiente de las cordilleras (caminarías por una ladera de
// 6 km como si fuera llana).
inline float sstepf(float e0, float e1, float x) {
    float t = glm::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

ValD geologyElevD(const glm::vec3& dir, const PlanetGeology& G) {
    const float v = G.sample(dir).upliftKm;
    // h angular pequeño pero no tanto que el float lo trague: ~0.002 rad ≈ 13 km en la Tierra.
    // La geología es de BAJA frecuencia (cordilleras de cientos de km) → esta escala la captura bien.
    const float h = 0.002f;
    glm::vec3 ref = (std::abs(dir.y) < 0.9f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    glm::vec3 t1  = glm::normalize(glm::cross(ref, dir));
    glm::vec3 t2  = glm::cross(dir, t1);
    const float a1 = G.sample(glm::normalize(dir + t1 * h)).upliftKm;
    const float b1 = G.sample(glm::normalize(dir - t1 * h)).upliftKm;
    const float a2 = G.sample(glm::normalize(dir + t2 * h)).upliftKm;
    const float b2 = G.sample(glm::normalize(dir - t2 * h)).upliftKm;
    const glm::vec3 g = t1 * ((a1 - b1) / (2.0f * h)) + t2 * ((a2 - b2) / (2.0f * h));
    return { v, g };
}

inline ValD operator+(const ValD& a, const ValD& b) { return { a.v + b.v, a.g + b.g }; }
inline ValD operator-(const ValD& a, const ValD& b) { return { a.v - b.v, a.g - b.g }; }
inline ValD operator+(const ValD& a, float s)       { return { a.v + s, a.g }; }
inline ValD operator-(const ValD& a, float s)       { return { a.v - s, a.g }; }
inline ValD operator*(const ValD& a, float s)       { return { a.v * s, a.g * s }; }
inline ValD operator*(const ValD& a, const ValD& b) { // regla del producto
    return { a.v * b.v, a.v * b.g + b.v * a.g };
}

// |x|: derivada = sign(x)·x'
inline ValD absD(const ValD& a) {
    float s = (a.v >= 0.0f) ? 1.0f : -1.0f;
    return { std::fabs(a.v), a.g * s };
}

// clamp a [lo,hi]: derivada 0 fuera del rango.
inline ValD clampD(const ValD& a, float lo, float hi) {
    if (a.v <= lo) return { lo, glm::vec3(0.0f) };
    if (a.v >= hi) return { hi, glm::vec3(0.0f) };
    return a;
}

// smoothstep(e0,e1,x) cúbico, con su derivada encadenada.
inline ValD smoothstepD(float e0, float e1, const ValD& x) {
    float inv = 1.0f / (e1 - e0);
    float t   = (x.v - e0) * inv;
    if (t <= 0.0f) return { 0.0f, glm::vec3(0.0f) };
    if (t >= 1.0f) return { 1.0f, glm::vec3(0.0f) };
    float val  = t * t * (3.0f - 2.0f * t);
    float dval = (6.0f * t - 6.0f * t * t) * inv; // d/dx
    return { val, x.g * dval };
}

// pow(x, p) con p constante (x debe ser >= 0 en los usos: ya viene clampeado).
inline ValD powD(const ValD& a, float p) {
    float val = std::pow(std::max(a.v, 0.0f), p);
    float d   = (a.v > 0.0f) ? p * std::pow(a.v, p - 1.0f) : 0.0f;
    return { val, a.g * d };
}

// mix(a, b, t) con t también ValD (la transición de costa aporta gradiente).
inline ValD mixD(const ValD& a, const ValD& b, const ValD& t) {
    // a + (b-a)·t
    ValD bma = b - a;
    return { a.v + bma.v * t.v,
             a.g + bma.g * t.v + bma.v * t.g };
}

// acos(x) con derivada: d/dx = -1/sqrt(1-x²). Guard cerca de ±1.
inline ValD acosD(const ValD& a) {
    float v = glm::clamp(a.v, -0.999999f, 0.999999f);
    float d = -1.0f / std::sqrt(1.0f - v * v);
    return { std::acos(v), a.g * d };
}

// fBm con derivada → ValD.
inline ValD fbmD(const glm::vec3& dir, int seed, int oct, float pers, float lac, float scale) {
    glm::vec3 grad;
    float val = NoiseGenerator::fBm_d(dir, grad, seed, oct, pers, lac, scale);
    return { val, grad };
}

constexpr float A_NORM = 0.45f; // amplitud típica del fBm normalizado (para el ridged)

// --- Hash entero determinista (mezcla tipo murmur/lowbias) ---
inline uint32_t uhash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
inline uint32_t uhash2(uint32_t a, uint32_t b) { return uhash(a ^ (uhash(b) + 0x9e3779b9u)); }
inline float    hash01(uint32_t x) { return float(uhash(x) & 0xFFFFFFu) / float(0x1000000); }

// --- Voronoi/cellular sobre la esfera → id de continente (partición determinista) ---
// Devuelve el id de la celda-semilla más cercana; d1/d2 = distancias 1ª/2ª (para bordes).
inline uint32_t voronoiId(const glm::vec3& dir, float density, uint32_t seed,
                          float& d1, float& d2) {
    glm::vec3 p = dir * density;
    glm::ivec3 base = glm::ivec3(glm::floor(p));
    d1 = d2 = 1e9f; uint32_t id = 0;
    for (int dk = -1; dk <= 1; ++dk)
    for (int dj = -1; dj <= 1; ++dj)
    for (int di = -1; di <= 1; ++di) {
        glm::ivec3 cell = base + glm::ivec3(di, dj, dk);
        uint32_t ch = uhash2((uint32_t)(cell.x * 73856093) ^ (uint32_t)(cell.y * 19349663)
                                                          ^ (uint32_t)(cell.z * 83492791), seed);
        glm::vec3 jit(float(uhash(ch + 1) & 0xFFFF) / 65535.0f,
                      float(uhash(ch + 2) & 0xFFFF) / 65535.0f,
                      float(uhash(ch + 3) & 0xFFFF) / 65535.0f);
        glm::vec3 fp = glm::vec3(cell) + jit;
        float dist = glm::length(p - fp);
        if (dist < d1) { d2 = d1; d1 = dist; id = ch; }
        else if (dist < d2) { d2 = dist; }
    }
    return id;
}

// Centro de sello de lago más cercano a `dir` (rejilla cellular jittered).
// Devuelve la dirección del centro y el hash de su celda (para sus atributos).
inline void nearestLakeCell(const glm::vec3& dir, float density, uint32_t seed,
                            glm::vec3& outCenter, uint32_t& outHash) {
    glm::vec3 p = dir * density;
    glm::ivec3 base = glm::ivec3(glm::floor(p));
    float bestDot = -2.0f; outHash = 0; outCenter = dir;
    for (int dk = -1; dk <= 1; ++dk)
    for (int dj = -1; dj <= 1; ++dj)
    for (int di = -1; di <= 1; ++di) {
        glm::ivec3 cell = base + glm::ivec3(di, dj, dk);
        uint32_t ch = uhash2((uint32_t)(cell.x * 73856093) ^ (uint32_t)(cell.y * 19349663)
                                                          ^ (uint32_t)(cell.z * 83492791), seed);
        glm::vec3 jit(float(uhash(ch + 1) & 0xFFFF) / 65535.0f,
                      float(uhash(ch + 2) & 0xFFFF) / 65535.0f,
                      float(uhash(ch + 3) & 0xFFFF) / 65535.0f);
        glm::vec3 cdir = glm::normalize(glm::vec3(cell) + jit);
        float dt = glm::dot(dir, cdir);
        if (dt > bestDot) { bestDot = dt; outCenter = cdir; outHash = ch; }
    }
}

// Elevación REGIONAL (km) en una dirección: solo la base de tierra (continentalness
// → curva), sin montañas ni detalle. Sirve de nivel de agua del lago en su centro,
// sin recursión al sampler completo.
inline float regionalElevKm(const glm::vec3& dir, const WorldGenParams& W) {
    float cont = NoiseGenerator::fBm(dir, (int)W.seed, 6, 0.5f, 2.0f, W.continentFreqA);
    float t = glm::clamp((cont - W.seaThreshold) / 0.3f, 0.0f, 1.0f);
    return (t * t * (3.0f - 2.0f * t)) * 1.0f; // landHeight ~1 km
}

// distToCoast (m, con signo: >0 tierra, <0 mar) a la línea de costa (c=c0). FASE 4: gradiente
// ANALÍTICO (fBm_d) en vez de 4 fBm por diferencias finitas → 1 evaluación (perf) + sin el epsilon.
// gmag = |gradiente TANGENCIAL| de la continentalidad. MISMA fórmula que terrain_gen.comp y water.frag
// (fBmGrad portado) → paridad exacta (verificable con `haruka_tests parity`). `R` = radio (m).
inline float distToCoastWorld(const glm::vec3& dir, float c, float c0,
                              const WorldGenParams& W, float R) {
    glm::vec3 g3;
    NoiseGenerator::fBm_d(dir, g3, (int)W.seed, 6, 0.5f, 2.0f, W.continentFreqA); // ∂c/∂dir analítico
    glm::vec3 gtan = g3 - glm::dot(g3, dir) * dir;   // parte tangencial (quita la componente radial)
    float gmag = glm::length(gtan);
    float ddir = (c - c0) / std::max(gmag, 1e-5f);
    return ddir * R; // → metros
}

} // namespace

WorldGenParams deriveWorldParams(uint32_t seed, double planetRadius) {
    (void)planetRadius;
    WorldGenParams W; W.seed = seed;

    // Escala de continentes: UN factor s gobierna count + freq + densidad (coherente).
    float s = hash01(seed ^ 0x9E3779B9u);
    W.continentCount = (int)std::lround(glm::mix(3.0f, 8.0f, s));
    W.continentFreqA = glm::mix(0.8f, 1.6f, s);
    // regiones Voronoi ≈ 4π·densidad² → densidad = sqrt(count/4π) clava ~count regiones.
    W.voronoiDensity = std::sqrt((float)W.continentCount / (4.0f * 3.14159265f));

    // Proporción mar/tierra: la elige la seed, acotada (nunca mundo degenerado).
    W.oceanFraction = glm::mix(0.45f, 0.75f, hash01(seed ^ 0x85EBCA6Bu));

    // ANCHO de costa por seed: costa corta/acantilado (0.35) ↔ larga/playa gradual (2.5). Escala las
    // rampas de distToCoast (tierra/lecho). Cada mundo tiene su carácter de costa.
    W.coastWidth = glm::mix(0.35f, 2.5f, hash01(seed ^ 0xC0A57Du));

    // Calibrar seaThreshold = cuantil oceanFraction de la continentalidad (CDF empírica).
    // Garantiza el ratio exacto sea cual sea la distribución del ruido.
    const int M = 2048;
    std::vector<float> samp; samp.reserve(M);
    const float ga = 2.39996323f; // ángulo áureo, esfera de Fibonacci → muestreo uniforme
    for (int i = 0; i < M; ++i) {
        float y = 1.0f - 2.0f * (i + 0.5f) / M;
        float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        float th = ga * i;
        glm::vec3 d(std::cos(th) * r, y, std::sin(th) * r);
        samp.push_back(NoiseGenerator::fBm(d, (int)seed, 6, 0.5f, 2.0f, W.continentFreqA));
    }
    size_t k = std::min(samp.size() - 1, (size_t)(W.oceanFraction * samp.size()));
    std::nth_element(samp.begin(), samp.begin() + k, samp.end());
    W.seaThreshold = samp[k];
    return W;
}

// ====================== Perfil LUNA (cráteres, sin mar) ======================
// Cuerpo gris y árido: base suave sobre la esfera (mares/tierras altas) + varias
// capas de CRÁTERES (cuenca + borde elevado). Todo con autodiff (ValD) → normales
// correctas. elev SIEMPRE > 0 (clamp) → el generador no emite agua. Se streamea por
// chunks/LOD EXACTAMENTE igual que un planeta (misma estructura).
static TerrainSample sampleMoon(const glm::vec3& dir, const WorldGenParams& W, double R) {
    ValD elev = fbmD(dir, (int)W.seed + 11, 4, 0.5f, 2.0f, 3.0f) * 0.20f + 1.20f; // ~1.0–1.4 km

    struct CL { float density, soff, rmin, rmax, ddiv; };
    const CL layers[] = {
        {  35.0f, 1.0f, 1.5f, 5.0f, 9.0f },   // cráteres grandes
        { 110.0f, 2.0f, 0.4f, 1.6f, 7.0f },   // medianos
        { 320.0f, 3.0f, 0.10f, 0.5f, 6.0f },  // pequeños
    };
    for (const CL& L : layers) {
        glm::vec3 cc; uint32_t ch;
        nearestLakeCell(dir, L.density, W.seed ^ (uint32_t)(L.soff * 2654435761u), cc, ch);
        float rKm   = glm::mix(L.rmin, L.rmax, hash01(ch));
        float depth = rKm / L.ddiv;
        float rAng  = rKm * 1000.0f / (float)R;
        if (rAng < 1e-6f) continue;
        ValD cosd = { glm::dot(dir, cc), cc };
        ValD dAng = acosD(cosd);
        ValD bowl = smoothstepD(rAng, 0.0f, dAng) * (-depth);                  // cuenca
        ValD rim  = (smoothstepD(rAng * 0.72f, rAng, dAng)
                   - smoothstepD(rAng, rAng * 1.30f, dAng)) * (depth * 0.45f); // borde elevado
        elev = elev + bowl + rim;
    }
    elev = clampD(elev, 0.05f, 1000.0f); // nunca bajo la esfera → sin agua

    glm::vec3 G    = elev.g;
    glm::vec3 Gtan = G - glm::dot(G, dir) * dir;
    float     rho  = (float)R + 1000.0f * elev.v;
    glm::vec3 nd   = rho * dir - 1000.0f * Gtan;
    glm::vec3 normal = glm::length(nd) > 1e-9f ? glm::normalize(nd) : dir;
    if (glm::dot(normal, dir) < 0.0f) normal = -normal;

    TerrainSample s;
    s.landMask     = 1.0f;
    s.elevKm       = elev.v;
    s.normal       = normal;
    s.continentId  = 0;
    s.wetness      = 0.0f;
    s.climate      = 0.4f;
    s.waterType    = WaterType::None;
    s.waterLevelKm = -1e30f;
    return s;
}

// ====================== Perfil GAS (gigante gaseoso) =========================
// Esfera LISA (sin relieve duro), apenas una ondulación mínima. Las bandas son de
// COLOR (en el shader). Sin agua. Se streamea como cualquier planeta.
static TerrainSample sampleGas(const glm::vec3& dir, const WorldGenParams& W, double R) {
    ValD elev = fbmD(dir, (int)W.seed + 7, 3, 0.5f, 2.0f, 6.0f) * 0.05f + 0.50f; // casi liso
    glm::vec3 G    = elev.g;
    glm::vec3 Gtan = G - glm::dot(G, dir) * dir;
    float     rho  = (float)R + 1000.0f * elev.v;
    glm::vec3 nd   = rho * dir - 1000.0f * Gtan;
    glm::vec3 normal = glm::length(nd) > 1e-9f ? glm::normalize(nd) : dir;
    if (glm::dot(normal, dir) < 0.0f) normal = -normal;

    TerrainSample s;
    s.landMask     = 1.0f;
    s.elevKm       = elev.v;
    s.normal       = normal;
    s.continentId  = 0;
    s.wetness      = 0.0f;
    s.climate      = 0.5f;
    s.waterType    = WaterType::None;
    s.waterLevelKm = -1e30f;
    return s;
}

float coastDistanceMeters(const glm::vec3& dirIn, const WorldGenParams& W, double planetRadius) {
    if (W.profile != 0) return 1e9f;   // moon/gas: sin mar → sin costa
    const glm::vec3 dir = glm::normalize(dirIn);
    // Continentalidad (misma fBm que el gate A). 1 fBm.
    const float c  = NoiseGenerator::fBm(dir, (int)W.seed, 6, 0.5f, 2.0f, W.continentFreqA);
    const float c0 = W.seaThreshold;
    // EARLY-OUT: si el chunk está claramente lejos de la línea de costa (|c-c0| grande), su distToCoast
    // es de decenas/cientos de km → jamás cae dentro de la huella de ningún chunk → nos saltamos el
    // gradiente (4 fBm). Umbral holgado (0.15) → seguro (a |c-c0|=0.15 la costa está a ~90+ km).
    if (std::fabs(c - c0) > 0.15f) return (c > c0 ? 1.0f : -1.0f) * 1e6f;
    // Solo en la FRANJA costera: distToCoast exacto por gradiente. +4 fBm (raro → coste acotado).
    return distToCoastWorld(dir, c, c0, W, (float)planetRadius);
}

TerrainSample sampleTerrainV2(const glm::vec3& dirIn, const WorldGenParams& W, double planetRadius) {
    const glm::vec3 dir = glm::normalize(dirIn);
    const int   seed    = (int)W.seed;
    const double R      = planetRadius;
    const float  kmToM  = 1000.0f;

    // Perfil del cuerpo: cada uno genera su superficie. terran (0) sigue abajo.
    if (W.profile == 1) return sampleMoon(dir, W, R);
    if (W.profile == 2) return sampleGas(dir, W, R);

    // ===================== A — GEOLOGÍA (F2: tectónica de placas) =====================
    // ANTES: la geografía salía de un fBm con un umbral (`seaThreshold`) → continentes = manchas de
    // ruido y montañas esparcidas. AHORA la base es TECTÓNICA: corteza continental vs oceánica, y el
    // relieve lo levanta el LÍMITE de placa (cordillera / arco volcánico / fosa / dorsal / rift).
    // Ver docs/guides/PLAN_TERRENO_V3.md §2.
    // La OROGENIA viene HORNEADA en el campo (no se evalúan las placas por muestra: era el grueso
    // del coste de regenerar terreno, y es innecesario — la altura ya sale del campo).
    const PlanetFields&  FL = fieldsFor(W.seed);
    const FieldSample    fsm = FL.sample(dir);
    // La ALTURA base ya no es una fórmula: es el campo EROSIONADO. Y a escala humana, la TESELA
    // MESO (~40 m/téxel), que ya trae detalle + erosión local (valles, cauces, taludes).
    // MESO (HARUKA_MESO=1): teselas erosionadas a ~40 m/téxel. Apagado por defecto hasta moverlo a
    // un worker con presupuesto — sin eso, generar teselas cuelga el arranque. Si está apagado no hay
    // teselas residentes y esto cae al macro, exactamente igual que la GPU → paridad en ambos casos.
    PlanetMeso& MS = planetMeso(W.seed, FL, planetRadius);       // MISMA caché que el generador de chunks
    ValD geoBase = mesoElevD(dir, MS);             // meso si la tesela está; si no, macro

    // Tierra/mar SALE de la geología: el nivel del mar es simplemente elev = 0. Se acabó el umbral
    // arbitrario sobre el ruido. La banda suave evita un acantilado de un vértice en la costa.
    // ⚠️ BANDA ESTRECHA (±2 m), no 120 m. Con una banda ancha, TODO el terreno entre 0 y 60 m
    // quedaba con landMask<0.5 → el clamp de costa lo hundía bajo el mar → la llanura costera
    // entera aparecía sumergida (y el agua z-fighteaba con el suelo). La costa es elev = 0.
    ValD landMask = smoothstepD(-0.002f, 0.002f, geoBase);   // 0 mar … 1 tierra

    // ===================== B — DETALLE, CONDICIONADO POR LA GEOLOGÍA ==================
    // El ruido NO desaparece: pasa a ser detalle **gobernado por los campos**, no ruido libre.

    // --- CAUCE (nivel C): el CAUDAL del campo TALLA el terreno ------------------------------
    // El campo macro (~40 km/celda) sabe por dónde va el río, pero a esa resolución no lo pisas.
    // Aquí el caudal se convierte en GEOMETRÍA: talla el cauce y aplana la vega alrededor. Es lo que
    // hace que el río se VEA al caminar, y que la vegetación de ribera tenga dónde ponerse.
    const float flow      = fsm.flow;
    const float riverW    = sstepf(0.30f, 0.62f, flow);          // 0 divisoria … 1 cauce principal
    const float valleyW   = sstepf(0.18f, 0.50f, flow);          // vega/fondo de valle (más ancho)
    const float carveKm   = riverW * 0.045f;                     // hasta 45 m de encajamiento

    // Colinas de frecuencia media: solo en tierra, y APLANADAS en el fondo del valle (una vega es
    // plana: el río la ha rellenado de sedimento).
    ValD hmn        = fbmD(dir, seed + 77, 5, 0.5f, 2.0f, 300.0f);
    ValD hills      = hmn * (1.0f / A_NORM);              // ~[-1,1]
    // (El MESO ya trae el detalle fino Y lo ha erosionado: colinas suaves aquí SOLO como variación
    //  de gran escala, muy bajas — si no, se sumarían al relieve que la erosión ya esculpió.)
    ValD hillRelief = hills * landMask * (0.12f * (1.0f - 0.85f * valleyW));   // km

    // Crestas RIDGED: **solo donde la tectónica construye montaña** (orogeny), no repartidas por
    // toda la tierra. Esto es lo que hace que las crestas SIGAN la cordillera en vez de salpicarla.
    ValD mn     = fbmD(dir, seed + 123, 5, 0.5f, 2.1f, 800.0f);
    ValD na     = mn * (1.0f / A_NORM);
    ValD form   = powD(clampD(konst(1.0f) - absD(na), 0.0f, 1.0f), 1.3f);  // cresta definida
    // La orogenia es de baja frecuencia → la tratamos como escalar (su pendiente ya la lleva geoBase).
    const float mtnAmp = fsm.orogeny * W.reliefStrength;   // ya viene con el signo aplicado
    ValD mountains = form * (1.4f * mtnAmp);            // km, encima del levantamiento tectónico

    // Relieve del LECHO oceánico (montes submarinos), solo mar adentro.
    ValD omn    = fbmD(dir, seed + 311, 5, 0.5f, 2.1f, 600.0f);
    ValD ona    = omn * (1.0f / A_NORM);
    ValD oform  = powD(clampD(konst(1.0f) - absD(ona), 0.0f, 1.0f), 1.5f);
    ValD seaRelief = (oform - konst(0.5f)) * ((1.0f - landMask.v) * 0.6f);   // km

    // Elevación final = TECTÓNICA + detalle. Sin clamps artificiales de costa: la línea de costa es
    // simplemente elev = 0, y es continua porque geoBase lo es.
    // El CAUCE ya lo talla la erosión del meso → aquí no se vuelve a restar (se duplicaría).
    ValD elevD = geoBase + hillRelief + mountains + seaRelief;

    // distToCoast: proxy a partir de la propia elevación (>0 tierra, <0 mar). Antes se calculaba con
    // un gradiente de la continentalidad (4 fBm extra); ahora sale gratis y es COHERENTE por
    // construcción con la costa que se dibuja.
    const float dc = elevD.v * 1000.0f;   // m con signo

    ValD elev = elevD;   // (la fusión mar↔tierra ya la hace la geología: la costa es elev = 0)
    // COSTA PURA POR landMask (baja freq, estable entre LODs): clamp de AMBOS lados. landMask>0.5
    // tierra (≥+5mm), landMask<0.5 océano (≤−5mm) → sin charcos ni islotes sub-celda que floten
    // entre LODs (flicker). Condición IDÉNTICA a la GPU (terrain_gen.comp) y water.frag oceanElevKm.
    if (landMask.v > 0.5f) elev = clampD(elev,  0.005f,  1e30f);
    else                   elev = clampD(elev, -1e30f, -0.005f);

    // ---- Continente + wetness (necesario para gatear los lagos) ----
    int   continentId = -1;
    float wetness     = 0.0f;
    if (landMask.v > 0.5f) {
        float d1, d2;
        uint32_t cid = voronoiId(dir, W.voronoiDensity, W.seed ^ 0xC2B2AE35u, d1, d2);
        continentId = (int)(cid & 0x7fffffff);
        wetness     = hash01(cid);                // [0,1] por continente
    }

    // ===================== Etapa 2.5 — LAGOS: cuencas REALES (F3) ============
    // ANTES: "sellos" colocados por un hash, que TALLABAN una cuenca donde tocara. Un lago no está
    // donde un hash dice: está donde la topografía NO DRENA. Ahora sale del relleno de depresiones
    // (priority-flood) del campo hidrológico → el lago está en la cuenca endorreica de verdad.
    const float lakeKm = fsm.waterKm;                     // cota del lago aquí (0 = no hay)
    const bool  hasLake = (lakeKm > 0.0f && lakeKm > elev.v);

    // ===================== Normal analítica =================================
    // Superficie radial ρ(dir)=R+1000·elev. n ∝ ρ·dir − 1000·∇_tan(elev).
    glm::vec3 grad = elev.g;                                // ∇ elev[km] respecto a dir (incluye el bowl)
    glm::vec3 Gtan = grad - glm::dot(grad, dir) * dir;      // parte tangencial
    float     rho  = (float)R + kmToM * elev.v;
    glm::vec3 nd   = rho * dir - kmToM * Gtan;
    glm::vec3 normal = glm::length(nd) > 1e-9f ? glm::normalize(nd) : dir;
    if (glm::dot(normal, dir) < 0.0f) normal = -normal;

    // ===================== Salida ===========================================
    TerrainSample s;
    s.landMask    = landMask.v;
    s.elevKm      = elev.v;
    s.normal      = normal;
    // (F2) distToCoast: la propia elevación con signo. Antes era un proxy sobre el ruido de
    // continentalidad (c - c0); ahora la costa ES elev = 0, así que esto es EXACTO por construcción.
    s.distToCoast = elev.v;   // km con signo: >0 tierra · <0 mar
    s.continentId = continentId;
    s.wetness     = wetness;
    s.climate     = glm::clamp(1.0f - std::fabs(dir.y), 0.0f, 1.0f); // 0 polos → 1 ecuador (legado)
    // (F5) El clima REAL sale del campo — el MISMO que muestrea el shader del terreno (planet.frag)
    // y el que usan los props. Si cada uno lo derivara por su cuenta, el bosque no coincidiría con
    // el verde que ves.
    s.tempC       = fsm.tempC;
    s.humidity    = fsm.humidity;
    s.flow        = fsm.flow;
    s.orogeny     = fsm.orogeny;

    // AGUA: sale del CAMPO, no de una regla ad-hoc. Lago si la cuenca no drena y su cota está por
    // encima del suelo; océano si el suelo procedural está bajo el nivel del mar.
    //
    // ⚠️ CLAVE (arregla el "mar bajo tierra"): esto se decide con la elevación PROCEDURAL, no con la
    // excavada. Un agujero que cavas tierra adentro NO está conectado al mar → no se llena de agua.
    // Antes la física preguntaba "¿estoy bajo el nivel del mar?" y cualquier hoyo profundo se volvía
    // océano, con física de nadar incluida.
    if (hasLake) {
        s.waterType    = WaterType::Lake;
        s.waterLevelKm = lakeKm;
    } else if (fsm.elevKm < 0.0f) {          // el CAMPO dice mar aquí (no la altura cavada)
        s.waterType    = WaterType::Ocean;
        s.waterLevelKm = 0.0f;
    } else {
        s.waterType    = WaterType::None;    // tierra: aquí NO hay agua aunque caves
        s.waterLevelKm = 0.0f;
    }
    return s;
}

} // namespace Haruka
