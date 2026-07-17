#include "core/terrain/planet_fields.h"
#include "core/terrain/planet_geology.h"

#include <algorithm>
#include <cmath>
#include <queue>

namespace Haruka {

namespace {

    // --- Cube-sphere: dirección <-> (cara, u, v) ------------------------------------------------
    // El planeta se discretiza en las 6 caras de un cubo proyectadas a la esfera. Es la MISMA
    // topología que ya usa el quadtree de chunks → los campos y los chunks hablan el mismo idioma.
    glm::vec3 faceUVtoDir(int face, float u, float v) {   // u,v ∈ [-1,1]
        switch (face) {
            case 0: return glm::normalize(glm::vec3( 1.0f,   -v,   -u));  // +X
            case 1: return glm::normalize(glm::vec3(-1.0f,   -v,    u));  // -X
            case 2: return glm::normalize(glm::vec3(    u, 1.0f,    v));  // +Y
            case 3: return glm::normalize(glm::vec3(    u,-1.0f,   -v));  // -Y
            case 4: return glm::normalize(glm::vec3(    u,   -v, 1.0f));  // +Z
            default:return glm::normalize(glm::vec3(   -u,   -v,-1.0f));  // -Z
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

    // Ruido de valor (colinas de la base; el detalle fino NO va aquí: el campo es macro).
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
        float a = 0.5f, sum = 0.0f, f = freq;
        for (int i = 0; i < oct; ++i) { sum += vnoise(p * f, s + (uint32_t)i * 71u) * a; a *= 0.5f; f *= 2.0f; }
        return sum;
    }

} // namespace

void PlanetFields::generate(uint32_t seed, const PlanetGeology& geo, int faceRes, int erosionIters) {
    m_seed = seed;
    m_res  = std::clamp(faceRes, 64, 1024);
    const int R = m_res;
    const size_t N = (size_t)6 * R * R;

    m_elev.assign(N, 0.0f);
    m_flow.assign(N, 0.0f);
    m_water.assign(N, 0.0f);
    m_oro.assign(N, 0.0f);

    auto idx = [R](int f, int i, int j) { return (size_t)f * R * R + (size_t)j * R + i; };
    auto cellDir = [R](int f, int i, int j) {
        const float u = ((i + 0.5f) / R) * 2.0f - 1.0f;
        const float v = ((j + 0.5f) / R) * 2.0f - 1.0f;
        return faceUVtoDir(f, u, v);
    };

    // --- 1) ELEVACIÓN BASE: tectónica + colinas -----------------------------------------------
    std::vector<glm::vec3> dirs(N);
    for (int f = 0; f < 6; ++f)
        for (int j = 0; j < R; ++j)
            for (int i = 0; i < R; ++i) {
                const size_t k = idx(f, i, j);
                const glm::vec3 d = cellDir(f, i, j);
                dirs[k] = d;
                const GeologySample gs = geo.sample(d);
                const float base = gs.upliftKm;
                // Horneamos la orogenia AQUÍ (una vez), no por vértice en el generador.
                m_oro[k] = gs.orogeny * std::max(0.0f, gs.convergence);
                const float land = glm::clamp(base / 0.12f, 0.0f, 1.0f);
                m_elev[k] = base + fbm(d, seed + 77u, 4, 3.0f) * 0.9f * land;   // colinas, solo en tierra
            }

    // Vecinos: 8-conectividad DENTRO de la cara, y a través de los bordes saltamos por DIRECCIÓN
    // (proyectamos la celda de fuera a la esfera y buscamos su cara). Sin esto, los ríos se cortarían
    // en las costuras del cubo.
    auto neighbor = [&](int f, int i, int j, int di, int dj, int& nf, int& ni, int& nj) {
        int x = i + di, y = j + dj;
        if (x >= 0 && x < R && y >= 0 && y < R) { nf = f; ni = x; nj = y; return; }
        const float u = ((x + 0.5f) / R) * 2.0f - 1.0f;
        const float v = ((y + 0.5f) / R) * 2.0f - 1.0f;
        const glm::vec3 d = faceUVtoDir(f, u, v);   // extrapola fuera de la cara y re-proyecta
        float nu, nv;
        dirToFaceUV(d, nf, nu, nv);
        ni = std::clamp((int)((nu * 0.5f + 0.5f) * R), 0, R - 1);
        nj = std::clamp((int)((nv * 0.5f + 0.5f) * R), 0, R - 1);
    };

    const int DI[8] = { 1, 1, 0,-1,-1,-1, 0, 1 };
    const int DJ[8] = { 0, 1, 1, 1, 0,-1,-1,-1 };

    // --- 2) RELLENO DE DEPRESIONES (priority-flood) --------------------------------------------
    // Una cuenca sin salida no puede drenar: el agua la LLENA hasta que desborda. De aquí salen los
    // lagos REALES (donde la topografía no drena), en vez de "sellos" colocados por un hash.
    // El mar es el nivel de base: todo lo que está bajo 0 es sumidero.
    std::vector<float> filled = m_elev;
    {
        using PQ = std::pair<float, size_t>;                       // (elev, celda)
        std::priority_queue<PQ, std::vector<PQ>, std::greater<PQ>> pq;
        std::vector<char> done(N, 0);
        for (size_t k = 0; k < N; ++k)
            if (m_elev[k] <= 0.0f) { done[k] = 1; filled[k] = m_elev[k]; pq.push({ m_elev[k], k }); }

        while (!pq.empty()) {
            auto [e, k] = pq.top(); pq.pop();
            const int f = (int)(k / ((size_t)R * R));
            const int j = (int)((k % ((size_t)R * R)) / R);
            const int i = (int)(k % R);
            for (int n = 0; n < 8; ++n) {
                int nf, ni, nj;
                neighbor(f, i, j, DI[n], DJ[n], nf, ni, nj);
                const size_t nk = idx(nf, ni, nj);
                if (done[nk]) continue;
                done[nk] = 1;
                // Si el vecino está por debajo del nivel de desborde, se INUNDA hasta él (lago).
                filled[nk] = std::max(m_elev[nk], e + 1e-5f);
                pq.push({ filled[nk], nk });
            }
        }
    }
    // Lago = donde hubo que rellenar por encima del terreno real.
    for (size_t k = 0; k < N; ++k)
        m_water[k] = (filled[k] > m_elev[k] + 1e-4f && filled[k] > 0.0f) ? filled[k] : 0.0f;

    // --- 3) RUTADO (D8) + ACUMULACIÓN DE CAUDAL -----------------------------------------------
    // Cada celda manda su agua a la vecina más baja. Procesando de ARRIBA a ABAJO, el caudal se
    // acumula: eso ES el río. (Sobre `filled` para que el agua atraviese los lagos y siga bajando.)
    std::vector<int>    down(N, -1);
    std::vector<size_t> order(N);
    for (size_t k = 0; k < N; ++k) order[k] = k;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return filled[a] > filled[b]; });

    for (size_t k = 0; k < N; ++k) {
        const int f = (int)(k / ((size_t)R * R));
        const int j = (int)((k % ((size_t)R * R)) / R);
        const int i = (int)(k % R);
        float best = filled[k]; int bestK = -1;
        for (int n = 0; n < 8; ++n) {
            int nf, ni, nj;
            neighbor(f, i, j, DI[n], DJ[n], nf, ni, nj);
            const size_t nk = idx(nf, ni, nj);
            if (filled[nk] < best) { best = filled[nk]; bestK = (int)nk; }
        }
        down[k] = bestK;
    }

    std::vector<float> acc(N, 1.0f);            // cada celda aporta su propia lluvia
    for (size_t oi = 0; oi < N; ++oi) {
        const size_t k = order[oi];
        if (down[k] >= 0) acc[(size_t)down[k]] += acc[k];
    }

    // --- 4) EROSIÓN HIDRÁULICA (stream power) + 5) TÉRMICA -------------------------------------
    // E = K · A^m · S^n  (A = caudal, S = pendiente). Es LA ecuación del relieve fluvial: talla
    // valles en V donde hay agua y pendiente, y deja las divisorias intactas → red dendrítica.
    // Nada de esto sale de sumar ruido.
    const float cellRad = 2.0f / R;             // tamaño angular de celda (aprox)
    const float Kero    = 0.35f;
    const float mExp    = 0.5f, nExp = 1.0f;
    // ⚠️ TALUS A ESCALA MACRO, no ángulo de reposo. Con celdas de ~40 km, el ángulo de reposo real
    // (~35°) NO aplica: eso es de pedreras a escala de metros. A 40 km/celda, un talus de 0.9 km/rad
    // = 7 m de desnivel máximo entre celdas separadas 40 km = pendiente 0.014% → APLANABA las
    // cordilleras (uplift 7.9 km → campo 2.9 km, medido). Los Andes suben ~7 km en ~150 km = 5%.
    // talus = pendiente% × R_km. A 500 (~8%) salían PAREDES casi verticales en la costa (salto de ~4
    // km entre celdas de 40 km) y los faldones de los chunks quedaban EXPUESTOS = cortinas negras
    // dentadas. 150 (~2.3%) da cordilleras de varios km con laderas caminables y sin acantilados.
    const float talus   = 150.0f;               // km/rad — pendiente macro máxima ~2.3% (Andes-like)

    for (int it = 0; it < erosionIters; ++it) {
        // Hidráulica: cada celda baja según su caudal y su pendiente hacia aguas abajo.
        for (size_t oi = 0; oi < N; ++oi) {
            const size_t k = order[oi];
            const int dk = down[k];
            if (dk < 0) continue;
            if (m_elev[k] <= 0.0f) continue;                    // el mar no se erosiona
            const float dz = m_elev[k] - m_elev[(size_t)dk];
            if (dz <= 0.0f) continue;
            const float S = dz / cellRad;
            const float A = acc[k] / (float)N;                  // caudal normalizado
            float e = Kero * std::pow(A, mExp) * std::pow(S, nExp) * cellRad;
            e = std::min(e, dz * 0.5f);                         // nunca invertir la pendiente
            m_elev[k] -= e;
            // Deposición aguas abajo (una parte del material arrancado se queda) → llanuras y deltas.
            m_elev[(size_t)dk] += e * 0.25f;
        }
        // Térmica: ninguna ladera aguanta más del ángulo de reposo → talud/pedrera al pie.
        for (int f = 0; f < 6; ++f)
            for (int j = 0; j < R; ++j)
                for (int i = 0; i < R; ++i) {
                    const size_t k = idx(f, i, j);
                    if (m_elev[k] <= 0.0f) continue;
                    for (int n = 0; n < 8; ++n) {
                        int nf, ni, nj;
                        neighbor(f, i, j, DI[n], DJ[n], nf, ni, nj);
                        const size_t nk = idx(nf, ni, nj);
                        const float dz = m_elev[k] - m_elev[nk];
                        const float maxDz = talus * cellRad;
                        if (dz > maxDz) {
                            const float move = (dz - maxDz) * 0.25f;
                            m_elev[k]  -= move;
                            m_elev[nk] += move;
                        }
                    }
                }
    }

    // Caudal normalizado [0,1] con escala log: el rango de caudales abarca órdenes de magnitud
    // (un arroyo vs el Amazonas) → en lineal solo se vería el río principal.
    // OJO con la normalización: acc >= 1 en TODA celda (su propia lluvia). Si se normaliza como
    // log(acc+1)/log(max+1), el suelo queda en ~0.06 y con un umbral bajo "todo es río" (pasó).
    // Con log(acc)/log(max), una celda que no recoge nada da 0 exacto → el río es lo que DESTACA.
    float maxAcc = 2.0f;
    for (size_t k = 0; k < N; ++k) maxAcc = std::max(maxAcc, acc[k]);
    const float invLog = 1.0f / std::log(maxAcc);
    for (size_t k = 0; k < N; ++k)
        m_flow[k] = glm::clamp(std::log(std::max(acc[k], 1.0f)) * invLog, 0.0f, 1.0f);

    computeClimate();   // AL FINAL: la sombra orográfica necesita las cordilleras ya erosionadas
}

// F5 — CLIMA DERIVADO. Nada de esto es ruido: sale de la geografía que ya existe.
//
//  · TEMPERATURA: latitud (|y| en la esfera) + gradiente vertical (−6.5 °C/km, el real).
//  · HUMEDAD: la trae el VIENTO desde el mar. Se marcha AGUAS ARRIBA DEL VIENTO desde cada celda:
//    si se llega al mar, la celda recibe aire húmedo; cada paso sobre tierra reseca un poco, y
//    **subir terreno descarga lluvia** (la masa de aire se enfría al ascender) → al otro lado de la
//    cordillera llega seca. Eso es la SOMBRA OROGRÁFICA, y es el detalle que hace decir "esto es un
//    planeta": el desierto está DETRÁS de las montañas, no donde lo ponga un hash.
//
// La marcha se hace en 3D sobre la esfera (rotando la dirección), NO en las uv de la cara: en uv,
// una trayectoria que cruza a otra cara del cubo se cortaría → costuras de bioma en los bordes.
void PlanetFields::computeClimate() {
    const int R = m_res;
    const size_t N = (size_t)6 * R * R;
    m_temp.assign(N, 15.0f);
    m_humid.assign(N, 0.5f);

    // Elevación bilineal en una dirección cualquiera (la traza del viento cae entre celdas).
    auto elevAt = [&](const glm::vec3& dir) {
        int f; float u, v;
        dirToFaceUV(glm::normalize(dir), f, u, v);
        const float fx = glm::clamp((u * 0.5f + 0.5f) * R - 0.5f, 0.0f, (float)R - 1.001f);
        const float fy = glm::clamp((v * 0.5f + 0.5f) * R - 0.5f, 0.0f, (float)R - 1.001f);
        const int i0 = (int)fx, j0 = (int)fy;
        const int i1 = std::min(i0 + 1, R - 1), j1 = std::min(j0 + 1, R - 1);
        const float tx = fx - i0, ty = fy - j0;
        auto at = [&](int i, int j) { return m_elev[(size_t)f * R * R + (size_t)j * R + i]; };
        const float e0 = at(i0, j0) * (1 - tx) + at(i1, j0) * tx;
        const float e1 = at(i0, j1) * (1 - tx) + at(i1, j1) * tx;
        return e0 * (1 - ty) + e1 * ty;
    };

    // Viento ZONAL (oeste→este, como los vientos dominantes): en cada punto la tangente "este" es
    // normalize(cross(eje, dir)). Se anula en los polos, donde tampoco hay advección que modelar.
    const glm::vec3 axis(0, 1, 0);
    const int   kSteps   = 48;                              // alcance de la advección (~48 celdas)
    const float stepRad  = (3.14159265f * 0.5f) / (float)R; // ≈ tamaño angular de una celda
    const float kDryLand = 0.012f;                          // resecado por paso sobre tierra
    const float kRainOut = 0.55f;                           // lluvia descargada por km de ASCENSO

    std::vector<glm::vec3> trace;
    trace.reserve(kSteps);

    for (int f = 0; f < 6; ++f)
        for (int j = 0; j < R; ++j)
            for (int i = 0; i < R; ++i) {
                const size_t k = (size_t)f * R * R + (size_t)j * R + i;
                const float u = ((i + 0.5f) / R) * 2.0f - 1.0f;
                const float v = ((j + 0.5f) / R) * 2.0f - 1.0f;
                const glm::vec3 d0 = glm::normalize(faceUVtoDir(f, u, v));
                const float elev0 = m_elev[k];

                // --- TEMPERATURA: latitud + gradiente vertical ------------------------------
                // La latitud va en GRADOS, no como |y| (=seno de la latitud). Con |y|² el planeta se
                // congelaba a media latitud: a 53° daba −5 °C (en la Tierra son ~10 °C) → más de un
                // cuarto de la tierra emergida salía NEVADA. El perfil zonal real se ajusta bien con
                // una parábola en grados: ~27 °C en el ecuador, ~10 °C a 53°, ~−21 °C en el polo.
                const float latDeg = glm::degrees(std::asin(glm::clamp(std::abs(d0.y), 0.0f, 1.0f)));
                // Perfil zonal REALISTA (= el que describe el comentario de arriba): ~27 °C ecuador,
                // ~10 °C a 53°, ~−21 °C polo. La base NO se sube para "calentar desiertos" (eso volvía
                // TODO el planeta <50° un desierto y mataba bosques/praderas): el calor del desierto lo
                // pone el BONO DE ARIDEZ de abajo, solo donde está SECO.
                m_temp[k] = (27.0f - 0.0060f * latDeg * latDeg)     // ~27 °C ecuador … −21 °C polo
                          - 6.5f * std::max(0.0f, elev0);          // −6.5 °C/km (el gradiente real)
                // La ARIDEZ calienta: sin nubes ni humedad que amortigüe, el desierto se abrasa de
                // día. Sin esto, un "desierto" a 20 °C — el usuario lo llamó irrisorio, con razón.
                // (Se aplica DESPUÉS de la humedad, ver abajo: aquí guardamos el término y lo sumamos
                // tras conocer m_humid[k].)

                if (elev0 <= 0.0f) { m_humid[k] = 1.0f; continue; }   // el mar ES la fuente

                // --- HUMEDAD: traza CONTRA el viento hasta el mar ---------------------------
                trace.clear();
                glm::vec3 d = d0;
                bool reachedSea = false;
                for (int s = 0; s < kSteps; ++s) {
                    glm::vec3 east = glm::cross(axis, d);
                    const float len = glm::length(east);
                    if (len < 1e-4f) break;                       // polo: sin viento zonal
                    d = glm::normalize(d - (east / len) * stepRad);
                    trace.push_back(d);
                    if (elevAt(d) <= 0.0f) { reachedSea = true; break; }
                }

                // Sin mar a la vista en todo el alcance = interior continental profundo → seco.
                float hum = 0.0f;
                if (reachedSea) {
                    // Ahora HACIA DELANTE, del mar a esta celda: el aire llega saturado y va
                    // perdiendo humedad al rozar tierra y, sobre todo, AL SUBIR (ahí llueve).
                    //
                    // ⚠️ La cota del aire se mide desde el NIVEL DEL MAR, no desde la batimetría:
                    // con la elevación cruda, el primer tramo "subía" desde el fondo oceánico
                    // (−3 km) y descargaba TODA la lluvia nada más tocar la costa → tierra adentro
                    // todo seco por igual y la sombra orográfica desaparecía (medido: 53% ≈ azar).
                    auto airElev = [](float e) { return std::max(0.0f, e); };
                    hum = 1.0f;
                    float prev = airElev(elevAt(trace.back()));   // el mar → 0
                    for (int s = (int)trace.size() - 2; s >= 0; --s) {
                        const float e = airElev(elevAt(trace[s]));
                        hum -= kDryLand;
                        hum -= kRainOut * std::max(0.0f, e - prev); // ASCENSO → descarga
                        hum = glm::clamp(hum, 0.0f, 1.0f);
                        prev = e;
                    }
                    hum -= kRainOut * std::max(0.0f, airElev(elev0) - prev); // hasta la celda
                    hum = glm::clamp(hum, 0.0f, 1.0f);
                }

                // Ríos y lagos riegan su entorno: un cauce nunca está en un desierto absoluto.
                m_humid[k] = glm::clamp(hum + 0.35f * m_flow[k] + (m_water[k] > 0.0f ? 0.3f : 0.0f),
                                        0.0f, 1.0f);
                // Bono de aridez a la temperatura: hasta +12 °C SOLO donde no llueve (desierto seco) →
                // el desierto se abrasa (~36-39 °C) mientras el resto conserva el perfil realista. Este
                // es el mecanismo que calienta desiertos, NO subir la base global (eso desertificaba el
                // planeta entero y mataba los bosques — regresión 2026-07).
                m_temp[k] += 12.0f * (1.0f - glm::smoothstep(0.05f, 0.35f, m_humid[k]));
            }
}

FieldSample PlanetFields::sample(const glm::vec3& dirIn) const {
    FieldSample s;
    if (m_elev.empty()) return s;

    const glm::vec3 d = glm::normalize(dirIn);
    int f; float u, v;
    dirToFaceUV(d, f, u, v);

    const int R = m_res;
    const float fx = glm::clamp((u * 0.5f + 0.5f) * R - 0.5f, 0.0f, (float)R - 1.001f);
    const float fy = glm::clamp((v * 0.5f + 0.5f) * R - 0.5f, 0.0f, (float)R - 1.001f);
    const int   i0 = (int)fx, j0 = (int)fy;
    const int   i1 = std::min(i0 + 1, R - 1), j1 = std::min(j0 + 1, R - 1);
    const float tx = fx - i0, ty = fy - j0;

    auto at = [&](const std::vector<float>& a, int i, int j) {
        return a[(size_t)f * R * R + (size_t)j * R + i];
    };
    auto bilinear = [&](const std::vector<float>& a) {
        const float e0 = at(a, i0, j0) * (1 - tx) + at(a, i1, j0) * tx;
        const float e1 = at(a, i0, j1) * (1 - tx) + at(a, i1, j1) * tx;
        return e0 * (1 - ty) + e1 * ty;
    };

    s.elevKm  = bilinear(m_elev);
    s.flow    = bilinear(m_flow);
    s.waterKm = bilinear(m_water);
    s.orogeny = bilinear(m_oro);
    s.tempC    = bilinear(m_temp);
    s.humidity = bilinear(m_humid);
    s.isLake  = s.waterKm > 0.0f && s.waterKm > s.elevKm;
    return s;
}

} // namespace Haruka
