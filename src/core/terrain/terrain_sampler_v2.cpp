#include "terrain_sampler_v2.h"
#include "core/noise_generator.h"

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

TerrainSample sampleTerrainV2(const glm::vec3& dirIn, const WorldGenParams& W, double planetRadius) {
    const glm::vec3 dir = glm::normalize(dirIn);
    const int   seed    = (int)W.seed;
    const double R      = planetRadius;
    const float  kmToM  = 1000.0f;

    // Perfil del cuerpo: cada uno genera su superficie. terran (0) sigue abajo.
    if (W.profile == 1) return sampleMoon(dir, W, R);
    if (W.profile == 2) return sampleGas(dir, W, R);

    // ===================== A — geografía (binario gate) =====================
    ValD c = fbmD(dir, seed, 6, 0.5f, 2.0f, W.continentFreqA); // continentalness ~[-1,1]
    const float c0 = W.seaThreshold;
    const float band = 0.04f; // medio-ancho de banda costa en unidades de ruido (suave, no 0)

    ValD landMask = smoothstepD(c0 - band, c0 + band, c); // 0 mar … 1 tierra

    // ===================== B — relieve (gateado por A) ======================
    // Rama MAR: profundidad crece según c baja de c0 (curva anclada a 0 en costa).
    ValD seaT     = smoothstepD(c0, c0 - 0.5f, c);          // 0 costa → 1 abismo
    ValD seaDepth = seaT * (-5.0f);                          // km (oceanDepth ~5)
    // Batimetría: relieve submarino (dorsales/montes + fosas) sobre el gradiente liso →
    // el lecho tiene relieve como la tierra (no un cuenco plano). Gateado por seaT (~0 en
    // la costa, crece mar adentro). MISMO cálculo que el compute GPU (terrain_gen.comp).
    ValD oreg   = fbmD(dir, seed + 211, 4, 0.5f, 2.0f, 3.0f);
    ValD oMask  = smoothstepD(0.05f, 0.30f, oreg);
    ValD omn    = fbmD(dir, seed + 311, 5, 0.5f, 2.1f, 600.0f);
    ValD ona    = omn * (1.0f / A_NORM);
    ValD oform  = powD(clampD(konst(1.0f) - absD(ona), 0.0f, 1.0f), 1.5f); // dorsales
    ValD oceanRelief = (oform - konst(0.5f)) * oMask * 3.0f;               // km [-1.5,1.5]
    seaDepth = clampD(seaDepth + oceanRelief * seaT, -1e9f, -0.02f);       // siempre bajo el mar

    // Rama TIERRA: base que sube desde la costa.
    ValD landT    = smoothstepD(c0, c0 + 0.3f, c);          // 0 costa → 1 tierra adentro
    ValD landBase = landT * 1.0f;                            // km (landHeight ~1)

    // Colinas de frecuencia MEDIA en TODA la tierra → relieve "normal" ondulado. Antes el
    // terreno era BIMODAL (llano de 1km o cresta ridged de 7.5km, sin nada intermedio); esto
    // rellena el tramo medio. fBm suave (NO ridged) → laderas redondeadas. Gateado por landT
    // (costa→0). MISMO código que el compute GPU (terrain_gen.comp) → paridad render↔colisión.
    ValD hmn        = fbmD(dir, seed + 77, 5, 0.5f, 2.0f, 300.0f); // freq alta → onda ~130km, VISIBLE
    ValD hills      = hmn * (1.0f / A_NORM);                 // ~[-1,1]
    ValD hillRelief = hills * landT * 1.1f;                  // km, ondulado ±~1.1

    // Régimen (fase 1): dónde hay montañas (vs llano/montículo).
    ValD reg     = fbmD(dir, seed + 55, 4, 0.5f, 2.0f, 2.0f);
    // Máscara ESTRECHA: montañas CONCENTRADAS en regiones (cordilleras), no esparcidas como
    // montículos por toda la tierra (antes -0.05→0.30 cubría casi todo → ruido de montaña en
    // todas partes). MISMO valor que el compute GPU.
    ValD mtnMask = smoothstepD(0.12f, 0.36f, reg);

    // Montañas RIDGED con MASA: exponente 1.3 → cresta definida. 5 octavas → sin spikes finos.
    // Pico 6.5 km (prominente) apoyado sobre las colinas → relieve continuo costa→colina→pico.
    ValD mn     = fbmD(dir, seed + 123, 5, 0.5f, 2.1f, 800.0f);
    ValD na     = mn * (1.0f / A_NORM);
    ValD form   = powD(clampD(konst(1.0f) - absD(na), 0.0f, 1.0f), 1.3f); // cresta definida, con masa
    ValD mountains = form * mtnMask * (6.5f * W.reliefStrength); // km. reliefStrength = parámetro de escena

    // La TIERRA nunca bajo el nivel del mar (las colinas la hundían bajo 0 → la esfera de
    // océano de órbita la tapaba → continentes disueltos en islas). Clamp a +10 m. MISMO que
    // el compute GPU.
    ValD landRelief = clampD(landBase + hillRelief + mountains, 0.01f, 1e30f);

    // Fusión costa: mar ↔ tierra por landMask (suave).
    ValD elev = mixD(seaDepth, landRelief, landMask);       // km
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

    // ===================== Etapa 2.5 — lagos (sellos de la seed) ============
    // Sellos deterministas gateados por wetness. Cada lago CONFORMA el terreno
    // (talla una cuenca) y fija un nivel de agua local. El bowl entra en el
    // gradiente (autodiff) → la normal de la orilla del lago es correcta.
    WaterType lakeWater       = WaterType::None;
    float     lakeLevel       = 0.0f;
    float     activeLakeLevel = -1e30f; // nivel del lago si estás DENTRO de un sello activo (para la orilla)
    if (landMask.v > 0.5f) {
        glm::vec3 lc; uint32_t lh;
        nearestLakeCell(dir, W.lakeDensity, W.seed ^ 0x1A4E1A4Eu, lc, lh);
        if (hash01(lh ^ 0x5151u) < wetness * W.lakeMaxProb) {     // activo (gateado por wetness)
            float rKm  = glm::mix(0.1f, 2.0f,  hash01(lh ^ 0x33u)); // 100 m … 2 km (charco→lago)
            float Dkm  = glm::mix(0.01f, 0.06f, hash01(lh ^ 0x77u)); // 10 … 60 m de cuenca
            float rAng = rKm * 1000.0f / (float)R;
            ValD cosd  = { glm::dot(dir, lc), lc };               // ∂(dir·lc)/∂dir = lc (lc fijo)
            ValD dAng  = acosD(cosd);
            ValD carve = smoothstepD(rAng, 0.0f, dAng) * Dkm;     // Dkm en el centro → 0 en el borde
            elev = elev - carve;                                  // tallar la cuenca
            float L = regionalElevKm(lc, W);                      // nivel del agua = suelo regional del centro
            activeLakeLevel = L;                                  // nivel aplicable (también en la orilla, elev>L)
            if (elev.v < L) { lakeWater = WaterType::Lake; lakeLevel = L; }
        }
    }

    // ===================== Normal analítica =================================
    // Superficie radial ρ(dir)=R+1000·elev. n ∝ ρ·dir − 1000·∇_tan(elev).
    glm::vec3 G    = elev.g;                                // ∇ elev[km] respecto a dir (incluye el bowl)
    glm::vec3 Gtan = G - glm::dot(G, dir) * dir;            // parte tangencial
    float     rho  = (float)R + kmToM * elev.v;
    glm::vec3 nd   = rho * dir - kmToM * Gtan;
    glm::vec3 normal = glm::length(nd) > 1e-9f ? glm::normalize(nd) : dir;
    if (glm::dot(normal, dir) < 0.0f) normal = -normal;

    // ===================== Salida ===========================================
    TerrainSample s;
    s.landMask    = landMask.v;
    s.elevKm      = elev.v;
    s.normal      = normal;
    s.distToCoast = (c.v - c0); // proxy normalizado (refinable a métrico)
    s.continentId = continentId;
    s.wetness     = wetness;
    s.climate     = glm::clamp(1.0f - std::fabs(dir.y), 0.0f, 1.0f); // 0 polos → 1 ecuador

    // Nivel de agua APLICABLE (para calcular orillas, también en tierra): lago si
    // estás dentro de un sello activo, si no el océano global (0).
    s.waterLevelKm = (activeLakeLevel > -1e29f) ? activeLakeLevel : 0.0f;
    // Tipo de agua (para la malla de agua): océano si el suelo baja de 0, o lago.
    if (s.elevKm < 0.0f)                         s.waterType = WaterType::Ocean;
    else if (lakeWater == WaterType::Lake)       s.waterType = WaterType::Lake;
    // else: tierra sobre el agua → waterType = None (pero waterLevelKm queda fijado).
    return s;
}

} // namespace Haruka
