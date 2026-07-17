#include "core/terrain/planet_meso.h"
#include "core/terrain/planet_fields.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <mutex>
#include <memory>
#include <cstdio>
#include <cstdlib>

namespace Haruka {

namespace {

    // Mismo mapeo cube-sphere que PlanetFields (tienen que hablar el mismo idioma).
    glm::vec3 faceUVtoDir(int face, float u, float v) {
        switch (face) {
            case 0: return glm::normalize(glm::vec3( 1.0f,   -v,   -u));
            case 1: return glm::normalize(glm::vec3(-1.0f,   -v,    u));
            case 2: return glm::normalize(glm::vec3(    u, 1.0f,    v));
            case 3: return glm::normalize(glm::vec3(    u,-1.0f,   -v));
            case 4: return glm::normalize(glm::vec3(    u,   -v, 1.0f));
            default:return glm::normalize(glm::vec3(   -u,   -v,-1.0f));
        }
    }
    void dirToFaceUV(const glm::vec3& d, int& face, float& u, float& v) {
        const float ax = std::abs(d.x), ay = std::abs(d.y), az = std::abs(d.z);
        if (ax >= ay && ax >= az) {
            if (d.x > 0) { face = 0; u = -d.z / ax; v = -d.y / ax; }
            else         { face = 1; u =  d.z / ax; v = -d.y / ax; }
        } else if (ay >= az) {
            if (d.y > 0) { face = 2; u =  d.x / ay; v =  d.z / ay; }
            else         { face = 3; u =  d.x / ay; v = -d.z / ay; }
        } else {
            if (d.z > 0) { face = 4; u =  d.x / az; v = -d.y / az; }
            else         { face = 5; u = -d.x / az; v = -d.y / az; }
        }
    }

    inline uint32_t hU(uint32_t x) {
        x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x;
    }
    inline uint32_t hC(uint32_t a, uint32_t b) { return hU(a ^ (b + 0x9e3779b9U + (a << 6) + (a >> 2))); }
    inline float h01(uint32_t h) { return (float)(h & 0xFFFFFF) / (float)0xFFFFFF; }
    float vh(const glm::ivec3& p, uint32_t s) {
        return h01(hC(hC((uint32_t)(p.x * 73856093) ^ (uint32_t)(p.y * 19349663),
                         (uint32_t)(p.z * 83492791)), s)) * 2.0f - 1.0f;
    }
    float vnoise(const glm::vec3& x, uint32_t s) {
        glm::vec3 i = glm::floor(x), f = x - i;
        f = f * f * (3.0f - 2.0f * f);
        glm::ivec3 I((int)i.x, (int)i.y, (int)i.z);
        auto L = [](float a, float b, float t) { return a + (b - a) * t; };
        float x00 = L(vh(I + glm::ivec3(0,0,0), s), vh(I + glm::ivec3(1,0,0), s), f.x);
        float x10 = L(vh(I + glm::ivec3(0,1,0), s), vh(I + glm::ivec3(1,1,0), s), f.x);
        float x01 = L(vh(I + glm::ivec3(0,0,1), s), vh(I + glm::ivec3(1,0,1), s), f.x);
        float x11 = L(vh(I + glm::ivec3(0,1,1), s), vh(I + glm::ivec3(1,1,1), s), f.x);
        return L(L(x00, x10, f.y), L(x01, x11, f.y), f.z);
    }
    float fbm(const glm::vec3& p, uint32_t s, int oct, float freq) {
        float a = 0.5f, sum = 0.0f, fr = freq;
        for (int i = 0; i < oct; ++i) { sum += vnoise(p * fr, s + (uint32_t)i * 131u) * a; a *= 0.5f; fr *= 2.0f; }
        return sum;
    }

    inline uint64_t packKey(int face, int tx, int ty) {
        return ((uint64_t)face << 48) | ((uint64_t)(uint32_t)tx << 24) | (uint64_t)(uint32_t)ty;
    }

} // namespace

// UNA sola instancia por seed: la comparten el generador de chunks (GPU) y el sampler de física
// (CPU). Ver la nota del header — dos cachés distintas romperían la paridad.
PlanetMeso& planetMeso(uint32_t seed, const PlanetFields& macro, double planetRadius) {
    static std::mutex mx;
    static std::vector<std::unique_ptr<PlanetMeso>> cache;
    static std::vector<uint32_t> seeds;
    std::lock_guard<std::mutex> lk(mx);
    for (size_t i = 0; i < seeds.size(); ++i) if (seeds[i] == seed) return *cache[i];
    auto m = std::make_unique<PlanetMeso>();
    m->init(seed, &macro, planetRadius);
    cache.push_back(std::move(m));
    seeds.push_back(seed);
    return *cache.back();
}

void PlanetMeso::init(uint32_t seed, const PlanetFields* macro, double planetRadius) {
    m_seed  = seed;
    m_macro = macro;
    // Nivel de tesela DERIVADO DEL RADIO: la cara del cubo abarca un arco de (pi/2)·R, y la tesela
    // es esa cara entre 2^level. Elegimos el level que deja la tesela lo más cerca de kTileTargetM.
    {
        const double faceArcM = (3.14159265358979 * 0.5) * std::max(1.0, planetRadius);
        const double want     = std::log2(std::max(1.0, faceArcM / kTileTargetM));
        m_tileLevel = std::clamp((int)std::lround(want), kMinTileLevel, kMaxTileLevel);
    }
    m_atlas.assign((size_t)kMaxTiles * kTileRes * kTileRes, 0.0f);
    m_table.assign(kMaxTiles, glm::ivec4(0));
    m_tiles.clear();
    m_dirty.clear();
    m_clock = 0;
    if (m_workers.empty()) {
        unsigned hw = std::thread::hardware_concurrency();
        unsigned n  = std::clamp(hw ? hw / 3u : 2u, 2u, 6u);   // sin ahogar a los workers de chunks
        for (unsigned i = 0; i < n; ++i) m_workers.emplace_back(&PlanetMeso::workerLoop, this);
    }
}

PlanetMeso::~PlanetMeso() {
    m_stop = true;
    m_cv.notify_all();
    for (auto& t : m_workers) if (t.joinable()) t.join();
}

// El worker construye teselas FUERA del hilo de generación de chunks. Una tesela = ~10 ms; hacerlo
// en línea colgaba el arranque. Aquí el coste es invisible: el mundo se ve (con el macro) mientras
// el detalle llega.
void PlanetMeso::workerLoop() {
    for (;;) {
        uint64_t key;
        {
            std::unique_lock<std::mutex> lk(m_qmx);
            m_cv.wait(lk, [&] { return m_stop || !m_queue.empty(); });
            if (m_stop) return;
            key = m_queue.front();
            m_queue.pop_front();
        }
        const int face = (int)(key >> 48);
        const int tx   = (int)((key >> 24) & 0xFFFFFF);
        const int ty   = (int)(key & 0xFFFFFF);

        { std::lock_guard<std::mutex> lk(m_mx);
          if (m_tiles.count(key)) continue; }     // otro la construyó mientras esperaba

        std::vector<float> data;
        buildTile(face, tx, ty, data);            // ~10 ms, SIN el mutex

        std::lock_guard<std::mutex> lk(m_mx);
        if (m_tiles.count(key)) continue;         // carrera: otro la publicó → tirar la nuestra
        publishTile(key, face, tx, ty, data);
    }
}

bool PlanetMeso::requestTile(const glm::vec3& d) {
    if (!m_macro) return false;
    int face; float u, v;
    dirToFaceUV(glm::normalize(d), face, u, v);
    const int TILES = 1 << m_tileLevel;
    const int tx = std::clamp((int)((u * 0.5f + 0.5f) * TILES), 0, TILES - 1);
    const int ty = std::clamp((int)((v * 0.5f + 0.5f) * TILES), 0, TILES - 1);
    const uint64_t key = packKey(face, tx, ty);

    {
        std::lock_guard<std::mutex> lk(m_mx);
        auto it = m_tiles.find(key);
        if (it != m_tiles.end()) { it->second.lastUse = ++m_clock; return true; }   // lista
    }
    {   // no está: la encolamos (sin duplicar) y devolvemos false SIN bloquear
        std::lock_guard<std::mutex> lk(m_qmx);
        if (std::find(m_queue.begin(), m_queue.end(), key) == m_queue.end()) {
            if (m_queue.size() < 256) m_queue.push_back(key);   // cota: no acumular trabajo muerto
        }
    }
    m_cv.notify_one();
    return false;
}

int PlanetMeso::pending() const {
    std::lock_guard<std::mutex> lk(m_qmx);
    return (int)m_queue.size();
}

int PlanetMeso::acquireSlot(uint64_t key) {
    // ¿Hay hueco libre?
    for (int s = 0; s < kMaxTiles; ++s)
        if (m_table[s].w == 0) return s;
    // LRU: tira la tesela usada hace más tiempo.
    uint64_t oldest = UINT64_MAX; uint64_t victim = 0; int slot = 0;
    for (const auto& [k, t] : m_tiles)
        if (t.lastUse < oldest) { oldest = t.lastUse; victim = k; slot = t.slot; }
    m_tiles.erase(victim);
    return slot;
}

// Construye y EROSIONA una tesela EN UN BUFFER PROPIO (kTileRes²). NO toca el atlas ni los mapas:
// así el worker puede construir (~10 ms) SIN el mutex, y la física —que consulta la altura por cada
// muestra— no se queda esperando. Aquí vive el anti-costuras (ver el .h).
void PlanetMeso::buildTile(int face, int tx, int ty, std::vector<float>& out) {
    const int R = kTileRes, M = kMargin, G = R + 2 * M;      // rejilla con margen
    const int TILES = 1 << m_tileLevel;
    const size_t N = (size_t)G * G;

    // uv de la tesela dentro de la cara ([-1,1])
    const float u0 = (float)tx / TILES * 2.0f - 1.0f;
    const float v0 = (float)ty / TILES * 2.0f - 1.0f;
    const float du = 2.0f / TILES;                            // ancho uv de la tesela

    std::vector<glm::vec3> dir(N);
    std::vector<float> elev(N), acc(N);

    // --- 1) Base: macro upsampleado + detalle ---------------------------------------------------
    for (int j = 0; j < G; ++j)
        for (int i = 0; i < G; ++i) {
            const float fu = u0 + du * ((i - M + 0.5f) / R);
            const float fv = v0 + du * ((j - M + 0.5f) / R);
            const glm::vec3 d = faceUVtoDir(face, fu, fv);
            const size_t k = (size_t)j * G + i;
            dir[k] = d;
            const FieldSample ms = m_macro->sample(d);
            const float land = glm::clamp(ms.elevKm / 0.02f, 0.0f, 1.0f);
            // Detalle a ESCALA HUMANA. La frecuencia estaba en 900 → celdas de ~7 km: variación tan
            // lenta que el mundo era una rampa lisa al caminar (medido: 2.4 m de relieve sobre 1 km).
            // 4000 baja la celda a ~1.6 km, y la octava más fina a ~100 m (justo por encima del
            // téxel de 40 m → sin aliasing). Más amplitud (0.13 → 0.30) para que haya DESNIVEL de
            // verdad; la erosión de abajo lo esculpe en valles/lomas en vez de dejar ruido pelado.
            const float det = fbm(d, m_seed + 4242u, 6, 4000.0f) * 0.30f * land;
            elev[k] = ms.elevKm + det;

            // CAUDAL HEREDADO: la celda no arranca solo con su lluvia, sino con el caudal que el
            // MACRO dice que pasa por aquí. Sin esto, un río grande nacería de la nada en el borde
            // de la tesela y moriría en el otro → cauces cortados entre teselas (el riesgo #1).
            const float inflow = std::pow(ms.flow, 3.0f) * 4000.0f;   // flow es log-normalizado
            acc[k] = 1.0f + inflow;
        }

    // --- 2) Relleno de depresiones (para que el agua pueda salir) ------------------------------
    std::vector<float> filled = elev;
    {
        using PQ = std::pair<float, size_t>;
        std::priority_queue<PQ, std::vector<PQ>, std::greater<PQ>> pq;
        std::vector<char> done(N, 0);
        // Nivel de base: el BORDE de la rejilla (con margen) y el mar.
        for (int j = 0; j < G; ++j)
            for (int i = 0; i < G; ++i) {
                const size_t k = (size_t)j * G + i;
                if (i == 0 || j == 0 || i == G - 1 || j == G - 1 || elev[k] <= 0.0f) {
                    done[k] = 1; pq.push({ elev[k], k });
                }
            }
        const int DI[8] = { 1, 1, 0,-1,-1,-1, 0, 1 };
        const int DJ[8] = { 0, 1, 1, 1, 0,-1,-1,-1 };
        while (!pq.empty()) {
            auto [e, k] = pq.top(); pq.pop();
            const int i = (int)(k % G), j = (int)(k / G);
            for (int n = 0; n < 8; ++n) {
                const int x = i + DI[n], y = j + DJ[n];
                if (x < 0 || y < 0 || x >= G || y >= G) continue;
                const size_t nk = (size_t)y * G + x;
                if (done[nk]) continue;
                done[nk] = 1;
                filled[nk] = std::max(elev[nk], e + 1e-6f);
                pq.push({ filled[nk], nk });
            }
        }
    }

    // --- 3) D8 + acumulación --------------------------------------------------------------------
    const int DI[8] = { 1, 1, 0,-1,-1,-1, 0, 1 };
    const int DJ[8] = { 0, 1, 1, 1, 0,-1,-1,-1 };
    std::vector<int> down(N, -1);
    std::vector<size_t> order(N);
    for (size_t k = 0; k < N; ++k) order[k] = k;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return filled[a] > filled[b]; });
    for (size_t k = 0; k < N; ++k) {
        const int i = (int)(k % G), j = (int)(k / G);
        float best = filled[k]; int bk = -1;
        for (int n = 0; n < 8; ++n) {
            const int x = i + DI[n], y = j + DJ[n];
            if (x < 0 || y < 0 || x >= G || y >= G) continue;
            const size_t nk = (size_t)y * G + x;
            if (filled[nk] < best) { best = filled[nk]; bk = (int)nk; }
        }
        down[k] = bk;
    }
    for (size_t oi = 0; oi < N; ++oi) {
        const size_t k = order[oi];
        if (down[k] >= 0) acc[(size_t)down[k]] += acc[k];
    }

    // --- 4) Erosión: stream power + térmica -----------------------------------------------------
    const float cellRad = du / R;                    // tamaño angular de celda
    const float Kero = 0.55f, mExp = 0.5f;
    const float talus = 1.1f;
    float maxAcc = 1.0f;
    for (size_t k = 0; k < N; ++k) maxAcc = std::max(maxAcc, acc[k]);

    for (int it = 0; it < 25; ++it) {
        for (size_t oi = 0; oi < N; ++oi) {
            const size_t k = order[oi];
            const int dk = down[k];
            if (dk < 0 || elev[k] <= 0.0f) continue;
            const float dz = elev[k] - elev[(size_t)dk];
            if (dz <= 0.0f) continue;
            const float S = dz / cellRad;
            const float A = acc[k] / maxAcc;
            float e = Kero * std::pow(A, mExp) * S * cellRad;
            e = std::min(e, dz * 0.5f);
            elev[k] -= e;
            elev[(size_t)dk] += e * 0.30f;           // deposición → vegas y conos aluviales
        }
        for (int j = 1; j < G - 1; ++j)
            for (int i = 1; i < G - 1; ++i) {
                const size_t k = (size_t)j * G + i;
                if (elev[k] <= 0.0f) continue;
                for (int n = 0; n < 8; ++n) {
                    const size_t nk = (size_t)(j + DJ[n]) * G + (i + DI[n]);
                    const float dz = elev[k] - elev[nk];
                    const float maxDz = talus * cellRad;
                    if (dz > maxDz) {
                        const float mv = (dz - maxDz) * 0.25f;
                        elev[k] -= mv; elev[nk] += mv;
                    }
                }
            }
    }

    // --- 5) Guardar SOLO el interior (el margen se TIRA: sus condiciones de contorno son falsas) --
    out.resize((size_t)R * R);
    for (int j = 0; j < R; ++j)
        for (int i = 0; i < R; ++i)
            out[(size_t)j * R + i] = elev[(size_t)(j + M) * G + (i + M)];
}

// Publica una tesela ya construida en el atlas. CALLER TIENE m_mx.
void PlanetMeso::publishTile(uint64_t key, int face, int tx, int ty, const std::vector<float>& data) {
    const int slot = acquireSlot(key);
    std::copy(data.begin(), data.end(), m_atlas.begin() + (size_t)slot * kTileRes * kTileRes);
    m_table[slot] = glm::ivec4(face, tx, ty, 1);
    m_dirty.push_back(slot);
    m_tiles[key] = { slot, ++m_clock };
}

void PlanetMeso::ensureTile(const glm::vec3& d) {
    if (!m_macro) return;
    int face; float u, v;
    dirToFaceUV(glm::normalize(d), face, u, v);
    const int TILES = 1 << m_tileLevel;
    const int tx = std::clamp((int)((u * 0.5f + 0.5f) * TILES), 0, TILES - 1);
    const int ty = std::clamp((int)((v * 0.5f + 0.5f) * TILES), 0, TILES - 1);
    const uint64_t key = packKey(face, tx, ty);

    {   // ¿ya está? (el worker puede haberla publicado)
        std::lock_guard<std::mutex> lk(m_mx);
        auto it = m_tiles.find(key);
        if (it != m_tiles.end()) { it->second.lastUse = ++m_clock; return; }
    }
    std::vector<float> data;
    buildTile(face, tx, ty, data);              // fuera del lock, como el worker
    std::lock_guard<std::mutex> lk(m_mx);
    if (m_tiles.count(key)) return;             // el worker se nos adelantó
    publishTile(key, face, tx, ty, data);
}

bool PlanetMeso::hasTile(const glm::vec3& d) const {
    if (!m_macro) return false;
    int face; float u, v;
    dirToFaceUV(glm::normalize(d), face, u, v);
    const int TILES = 1 << m_tileLevel;
    const int tx = std::clamp((int)((u * 0.5f + 0.5f) * TILES), 0, TILES - 1);
    const int ty = std::clamp((int)((v * 0.5f + 0.5f) * TILES), 0, TILES - 1);
    return m_tiles.count(packKey(face, tx, ty)) != 0;
}

float PlanetMeso::elevKmIfResident(const glm::vec3& dirIn) const {
    if (!m_macro) return 0.0f;
    std::lock_guard<std::mutex> lk(m_mx);   // el worker puede estar escribiendo el atlas
    const glm::vec3 d = glm::normalize(dirIn);
    int face; float u, v;
    dirToFaceUV(d, face, u, v);
    const int TILES = 1 << m_tileLevel, R = kTileRes;
    const float gx = (u * 0.5f + 0.5f) * TILES * R;
    const float gy = (v * 0.5f + 0.5f) * TILES * R;
    const int tx = std::clamp((int)(gx / R), 0, TILES - 1);
    const int ty = std::clamp((int)(gy / R), 0, TILES - 1);

    auto it = m_tiles.find(packKey(face, tx, ty));
    if (it == m_tiles.end()) return m_macro->sample(d).elevKm;   // sin tesela → macro (igual que la GPU)
    const float* src = &m_atlas[(size_t)it->second.slot * R * R];
    const float fx = glm::clamp(gx - tx * R - 0.5f, 0.0f, (float)R - 1.001f);
    const float fy = glm::clamp(gy - ty * R - 0.5f, 0.0f, (float)R - 1.001f);
    const int i0 = (int)fx, j0 = (int)fy;
    const int i1 = std::min(i0 + 1, R - 1), j1 = std::min(j0 + 1, R - 1);
    const float sx = fx - i0, sy = fy - j0;
    const float e0 = src[(size_t)j0 * R + i0] * (1 - sx) + src[(size_t)j0 * R + i1] * sx;
    const float e1 = src[(size_t)j1 * R + i0] * (1 - sx) + src[(size_t)j1 * R + i1] * sx;
    return e0 * (1 - sy) + e1 * sy;
}

float PlanetMeso::elevKm(const glm::vec3& dirIn) {
    if (!m_macro) return 0.0f;
    const glm::vec3 d = glm::normalize(dirIn);
    ensureTile(d);

    int face; float u, v;
    dirToFaceUV(d, face, u, v);
    const int TILES = 1 << m_tileLevel, R = kTileRes;
    const float gx = (u * 0.5f + 0.5f) * TILES * R;      // coordenada de celda GLOBAL de la cara
    const float gy = (v * 0.5f + 0.5f) * TILES * R;
    const int tx = std::clamp((int)(gx / R), 0, TILES - 1);
    const int ty = std::clamp((int)(gy / R), 0, TILES - 1);

    auto it = m_tiles.find(packKey(face, tx, ty));
    if (it == m_tiles.end()) return m_macro->sample(d).elevKm;   // fallback (no debería pasar)
    const float* src = &m_atlas[(size_t)it->second.slot * R * R];

    // Bilineal dentro de la tesela (en el borde repite: el vecino es continuo porque ambos parten
    // del MISMO macro y del MISMO caudal heredado).
    const float fx = glm::clamp(gx - tx * R - 0.5f, 0.0f, (float)R - 1.001f);
    const float fy = glm::clamp(gy - ty * R - 0.5f, 0.0f, (float)R - 1.001f);
    const int i0 = (int)fx, j0 = (int)fy;
    const int i1 = std::min(i0 + 1, R - 1), j1 = std::min(j0 + 1, R - 1);
    const float sx = fx - i0, sy = fy - j0;
    const float e0 = src[(size_t)j0 * R + i0] * (1 - sx) + src[(size_t)j0 * R + i1] * sx;
    const float e1 = src[(size_t)j1 * R + i0] * (1 - sx) + src[(size_t)j1 * R + i1] * sx;
    return e0 * (1 - sy) + e1 * sy;
}

// MISMO hash que el compute (terrain_gen.comp::sampleMeso). Si divergen, la GPU busca en otro sitio.
static inline uint32_t mesoHash(int f, int tx, int ty) {
    uint32_t h = (uint32_t)f * 73856093u ^ (uint32_t)tx * 19349663u ^ (uint32_t)ty * 83492791u;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12;
    return h;
}

void PlanetMeso::gpuSnapshot(std::vector<glm::ivec4>& outTable,
                             std::vector<int>& outDirtySlots,
                             std::vector<float>& outDirtyData) {
    std::lock_guard<std::mutex> lk(m_mx);      // el worker puede estar escribiendo AHORA MISMO
    // Tabla HASH (sondeo lineal) en vez de la lista de slots: el compute la barría entera por
    // vértice, lo que ataba el cupo de teselas al coste del shader.
    outTable.assign(kHashSize, glm::ivec4(0));
    for (int s = 0; s < kMaxTiles; ++s) {
        const glm::ivec4& t = m_table[s];
        if (t.w == 0) continue;                                  // slot vacío
        uint32_t idx = mesoHash(t.x, t.y, t.z) & (uint32_t)(kHashSize - 1);
        for (int probe = 0; probe < 32; ++probe) {               // mismo sondeo que el shader
            if (outTable[idx].w == 0) { outTable[idx] = glm::ivec4(t.x, t.y, t.z, s + 1); break; }
            idx = (idx + 1) & (uint32_t)(kHashSize - 1);
        }
    }
    outDirtySlots.swap(m_dirty);
    m_dirty.clear();
    const size_t tileFloats = (size_t)kTileRes * kTileRes;
    outDirtyData.resize(outDirtySlots.size() * tileFloats);
    for (size_t i = 0; i < outDirtySlots.size(); ++i)
        std::copy_n(m_atlas.begin() + (size_t)outDirtySlots[i] * tileFloats, tileFloats,
                    outDirtyData.begin() + i * tileFloats);
}

} // namespace Haruka
