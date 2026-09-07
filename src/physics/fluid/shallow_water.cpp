#include "core/planet/ocean_wave.h"   // oceanWaterDisabled: el interruptor de biseccion
#include "shallow_water.h"
#include <algorithm>
#include <cmath>
#include <queue>      // priority_queue: la siembra de lagos (priority-flood)
#include <utility>
#include <vector>
#include "core/logger.h"

namespace Haruka::fluid {

void ShallowWaterSim::init(const glm::dvec3& anchor,
                           const glm::dvec3& tangent,
                           const glm::dvec3& bitangent,
                           const glm::dvec3& up,
                           double spanM, int n,
                           std::function<double(const glm::dvec3&)> terrainHeight,
                           std::function<double(const glm::dvec3&)> bakedWaterLevel) {
    m_n = std::max(2, n);
    m_span = spanM;
    m_dx = spanM / double(m_n - 1);
    m_anchor = anchor;
    m_tan = glm::normalize(tangent);
    m_bit = glm::normalize(bitangent);
    m_up  = glm::normalize(up);

    const int N = m_n * m_n;
    m_terrain.assign(N, 0.0f);
    m_water.assign(N, 0.0f);
    m_fL.assign(N, 0.0f); m_fR.assign(N, 0.0f);
    m_fB.assign(N, 0.0f); m_fT.assign(N, 0.0f);

    // Sample terrain height at each cell. We store RELATIVE elevation (vs the
    // anchor's terrain height) so flow only depends on local slope and the
    // numbers stay small/precise.
    double h0 = terrainHeight(anchor);
    for (int j = 0; j < m_n; ++j)
        for (int i = 0; i < m_n; ++i) {
            glm::dvec3 wp = worldPosAt(i, j);
            m_terrain[idx(i,j)] = float(terrainHeight(wp) - h0);
        }

    // ── LA SIEMBRA, DEL CAMPO DEL MUNDO SI LO HAY ──────────────────────────────────────────────
    //
    // ⚠️ `seedLakes` DEPENDE DEL BORDE DEL PARCHE, y su propia nota lo dice: una cuenca que asome por
    // él se considera abierta y no se llena, así que un lago APARECE al caminar 160 m. Con el campo
    // horneado (`TerrestrialPlanet::bakeWaterMap`, un priority-flood sobre el planeta ENTERO) esa
    // dependencia desaparece: el parche ya no decide dónde hay agua, sólo la mueve.
    //
    // El respaldo se queda para quien no tenga planeta horneado (los tests de la sim, un mundo plano).
    // ⚠️ EL PARCHE TAMBIEN OBEDECE A `HARUKA_NOWATER`. Se apagaba solo el mar y los lagos del CAMPO
    // (`m_hasWater`), asi que la sim de rios/lagos seguia sembrando agua y "quitar el agua" no la
    // quitaba entera — que es lo que reporto Andoni.
    if (Haruka::Planet::oceanWaterDisabled()) {
        HARUKA_LOGD("Fluid", "agua APAGADA por HARUKA_NOWATER: no se siembra nada");
        return;
    }
    if (bakedWaterLevel) {
        int filled = 0;
        for (int j = 0; j < m_n; ++j)
            for (int i = 0; i < m_n; ++i) {
                const double lv = bakedWaterLevel(worldPosAt(i, j));
                if (lv <= -1.0e29) continue;              // centinela de `WATER_FILL_DRY`
                // El campo viene en cota ABSOLUTA y el parche trabaja en relativa al ancla: la misma
                // resta `h0` que se acaba de aplicar al terreno, o el lago saldría desplazado.
                const float d = (float)(lv - h0) - m_terrain[idx(i,j)];
                if (d > 0.0f) { m_water[idx(i,j)] = d; ++filled; }
            }
        HARUKA_LOGD("Fluid", "agua sembrada del campo del MUNDO: %d celdas de %d (%.1f%%)",
                    filled, N, N ? 100.0 * filled / N : 0.0);
    } else {
        seedLakes();
    }
}

// ── SIEMBRA DE LAGOS: llena las hondonadas hasta su punto de derrame ────────────────────────────
//
// Antes de esto la simulación arrancaba SECA y solo había dos fuentes de agua en todo el motor:
// `addRain` (únicamente si llovía) y `applySeaLevel` (que rellena la costa, y el renderizador la
// descarta a propósito para no duplicar la lámina del océano). `addWater` no la llamaba nadie. O sea
// que el sistema era un acumulador de lluvia: sin lluvia, no había lagos ni ríos NUNCA.
//
// El algoritmo es PRIORITY-FLOOD, el estándar para "rellenar depresiones" en un heightfield:
//   1. Todas las celdas del BORDE entran en un montículo por altura de terreno. El borde es el
//      escape: lo que llegue ahí se va del parche.
//   2. Se saca la más baja y se propaga a sus vecinos: el nivel de agua de un vecino es el MÁXIMO
//      entre el nivel desde el que llegamos y su propio terreno. Ese máximo ES el punto de derrame.
//   3. El agua de cada celda es `nivel − terreno`.
//
// Una pasada O(n² log n) sobre 96×96, solo al re-anclar (cada 160 m). Comparado con las 9216
// llamadas al terreno procedural que ya cuesta ese re-anclaje, esto es ruido.
//
// ⚠️ LIMITACIÓN HONESTA: el resultado depende del BORDE del parche, porque el borde es por donde el
// agua escapa. Una cuenca que asome por el borde se considera abierta y no se llena; al re-anclar
// 160 m más allá puede quedar dentro y sí llenarse. O sea que un lago cerca del borde puede aparecer
// o desaparecer al caminar. Los del centro (~100 m) son estables. Arreglarlo de verdad pide un
// campo de lagos GLOBAL horneado con el planeta, no un parche local.
void ShallowWaterSim::seedLakes() {
    const int n = m_n;
    if (n < 3) return;
    const int N = n * n;

    // Umbral mínimo de profundidad: sin él, cada dolina de una celda se llena y el resultado es un
    // moteado de charcos de milímetros por todo el parche — ruido, no lagos.
    const float kMinDepth = 0.25f;

    std::vector<float> level(N);
    std::vector<char>  done(N, 0);
    // Montículo por nivel (el más BAJO primero): pair<nivel, celda>.
    std::priority_queue<std::pair<float,int>, std::vector<std::pair<float,int>>,
                        std::greater<std::pair<float,int>>> heap;

    auto push = [&](int i, int j) {
        const int c = idx(i,j);
        if (done[c]) return;
        done[c]  = 1;
        level[c] = m_terrain[c];        // el borde no retiene: su nivel es su propio terreno
        heap.emplace(level[c], c);
    };
    for (int i = 0; i < n; ++i) { push(i, 0); push(i, n-1); }
    for (int j = 1; j < n-1; ++j) { push(0, j); push(n-1, j); }

    while (!heap.empty()) {
        const auto top = heap.top(); heap.pop();
        const float lv = top.first;
        const int   c  = top.second;
        const int   ci = c % n, cj = c / n;
        const int di[4] = { 1, -1, 0, 0 }, dj[4] = { 0, 0, 1, -1 };
        for (int k = 0; k < 4; ++k) {
            const int ni = ci + di[k], nj = cj + dj[k];
            if (ni < 0 || nj < 0 || ni >= n || nj >= n) continue;
            const int nc = idx(ni, nj);
            if (done[nc]) continue;
            done[nc] = 1;
            // El nivel del vecino no puede bajar del que traemos: si su terreno es más bajo, queda
            // SUMERGIDO hasta el punto de derrame por el que hemos llegado. Eso es el lago.
            level[nc] = std::max(lv, m_terrain[nc]);
            heap.emplace(level[nc], nc);
        }
    }

    int filled = 0;
    for (int c = 0; c < N; ++c) {
        const float d = level[c] - m_terrain[c];
        if (d > kMinDepth) { m_water[c] = d; ++filled; }
    }
    if (filled > 0)
        HARUKA_LOGD("Fluid", "lagos sembrados: %d celdas de %d (%.1f%% del parche)",
                    filled, N, 100.0 * filled / N);
}

glm::dvec3 ShallowWaterSim::worldPosAt(int i, int j) const {
    double half = m_span * 0.5;
    double x = -half + double(i) * m_dx;
    double z = -half + double(j) * m_dx;
    return m_anchor + m_tan * x + m_bit * z;
}

void ShallowWaterSim::addWater(int i, int j, float depth) {
    if (i < 0 || j < 0 || i >= m_n || j >= m_n) return;
    m_water[idx(i,j)] = std::max(0.0f, m_water[idx(i,j)] + depth);
}

void ShallowWaterSim::addRain(float depth) {
    for (auto& w : m_water) w += depth;
}

void ShallowWaterSim::addSpringWorld(const glm::dvec3& worldPos, float radiusM,
                                     float ratePerSec, float dt) {
    // Project world pos onto the grid plane.
    glm::dvec3 d = worldPos - m_anchor;
    double x = glm::dot(d, m_tan);
    double z = glm::dot(d, m_bit);
    double half = m_span * 0.5;
    int ci = int((x + half) / m_dx + 0.5);
    int cj = int((z + half) / m_dx + 0.5);
    int r  = std::max(1, int(radiusM / m_dx));
    for (int j = cj - r; j <= cj + r; ++j)
        for (int i = ci - r; i <= ci + r; ++i) {
            if (i < 0 || j < 0 || i >= m_n || j >= m_n) continue;
            double dd = double((i-ci)*(i-ci) + (j-cj)*(j-cj));
            if (dd <= double(r*r)) addWater(i, j, ratePerSec * dt);
        }
}

bool ShallowWaterSim::containsWorld(const glm::dvec3& wp) const {
    glm::dvec3 d = wp - m_anchor;
    double x = glm::dot(d, m_tan), z = glm::dot(d, m_bit);
    double half = m_span * 0.5;
    return (x >= -half && x <= half && z >= -half && z <= half);
}

bool ShallowWaterSim::addVolumeAtWorld(const glm::dvec3& wp, float volumeM3) {
    glm::dvec3 d = wp - m_anchor;
    double x = glm::dot(d, m_tan), z = glm::dot(d, m_bit);
    double half = m_span * 0.5;
    int ci = int((x + half) / m_dx + 0.5);
    int cj = int((z + half) / m_dx + 0.5);
    if (ci < 0 || cj < 0 || ci >= m_n || cj >= m_n) return false;
    // depth = volume / cellArea
    float depth = volumeM3 / float(m_dx * m_dx);
    addWater(ci, cj, depth);
    return true;
}

float ShallowWaterSim::surfaceAlongUpAtWorld(const glm::dvec3& wp) const {
    glm::dvec3 d = wp - m_anchor;
    double x = glm::dot(d, m_tan), z = glm::dot(d, m_bit);
    double half = m_span * 0.5;
    int ci = int((x + half) / m_dx + 0.5);
    int cj = int((z + half) / m_dx + 0.5);
    if (ci < 0 || cj < 0 || ci >= m_n || cj >= m_n) return -1e9f;
    int c = idx(ci, cj);
    // Guard contra el TAMAÑO real (no solo m_n): si la sim quedó vacía/movida
    // (m_n>0 pero vectores sin datos), no accedas fuera de rango → trata como seco.
    if (c < 0 || c >= (int)m_terrain.size() || c >= (int)m_water.size()) return -1e9f;
    return m_terrain[c] + m_water[c];
}

float ShallowWaterSim::waterAtWorld(const glm::dvec3& wp) const {
    // Misma proyección que surfaceAlongUpAtWorld, pero devuelve la LÁMINA y no la superficie.
    // Existe porque comparar dos parches por su superficie no distingue el agua del terreno: en
    // pendiente, dos rejillas desplazadas muestrean el terreno en puntos distintos y la diferencia
    // de superficies es casi toda relieve. La profundidad no tiene ese problema — es 0 en seco,
    // caiga donde caiga la celda.
    glm::dvec3 d = wp - m_anchor;
    double x = glm::dot(d, m_tan), z = glm::dot(d, m_bit);
    double half = m_span * 0.5;
    int ci = int((x + half) / m_dx + 0.5);
    int cj = int((z + half) / m_dx + 0.5);
    if (ci < 0 || cj < 0 || ci >= m_n || cj >= m_n) return -1.0f;   // fuera del parche
    int c = idx(ci, cj);
    if (c < 0 || c >= (int)m_water.size()) return -1.0f;
    return m_water[c];
}

void ShallowWaterSim::applySeaLevel(float seaLevelAlongUp) {
    m_seaLevelAlongUp = seaLevelAlongUp; // F5.3: el render salta las celdas a/bajo este nivel (las cubre el océano)
    // For any cell whose terrain sits at/below sea level, force the water surface
    // (terrain + water) up to sea level — the ocean "fills" coastal cells. Above
    // sea level, leave the simulation alone (rivers flow freely).
    for (int c = 0; c < m_n * m_n; ++c) {
        if (m_terrain[c] <= seaLevelAlongUp) {
            float needed = seaLevelAlongUp - m_terrain[c];
            if (m_water[c] < needed) m_water[c] = needed;
        }
    }
}

double ShallowWaterSim::totalWater() const {
    double area = m_dx * m_dx, sum = 0.0;
    for (float w : m_water) sum += double(w) * area;
    return sum;
}

void ShallowWaterSim::step(float dt) {
    if (m_n < 2) return;
    // CFL: gravity wave speed ~ sqrt(g*h). Sub-step so flow stays stable.
    // Cap sub-steps; with shallow water and clamped flux this is robust.
    const float maxStep = float(0.25 * m_dx / std::sqrt(9.81 * 5.0 + 1e-3));
    int nsub = std::max(1, std::min(8, int(std::ceil(dt / maxStep))));
    float h = dt / float(nsub);
    for (int s = 0; s < nsub; ++s) substep(h);
}

void ShallowWaterSim::substep(float dt) {
    const int n = m_n;
    const float g = 9.81f;
    const float A = float(m_dx);            // pipe cross-section proxy
    const float l = float(m_dx);            // pipe length
    const float dx2 = float(m_dx * m_dx);   // cell area

    auto totalH = [&](int i, int j) {
        return m_terrain[idx(i,j)] + m_water[idx(i,j)];
    };

    // 1. Update outgoing flux from each cell to its 4 neighbours.
    //    f += dt * A * g * (ΔtotalHeight) / l ; clamped >= 0.
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            int c = idx(i,j);
            float hC = totalH(i,j);
            float fl = (i > 0)     ? std::max(0.0f, m_fL[c] + dt * A * g * (hC - totalH(i-1,j)) / l) : 0.0f;
            float fr = (i < n-1)   ? std::max(0.0f, m_fR[c] + dt * A * g * (hC - totalH(i+1,j)) / l) : 0.0f;
            float fb = (j > 0)     ? std::max(0.0f, m_fB[c] + dt * A * g * (hC - totalH(i,j-1)) / l) : 0.0f;
            float ft = (j < n-1)   ? std::max(0.0f, m_fT[c] + dt * A * g * (hC - totalH(i,j+1)) / l) : 0.0f;

            // 2. Scale so total outflow volume can't exceed the water present
            //    (this is what guarantees mass conservation / no negative depth).
            float outVol = (fl + fr + fb + ft) * dt;
            float avail  = m_water[c] * dx2;
            if (outVol > avail && outVol > 1e-12f) {
                float k = avail / outVol;
                fl *= k; fr *= k; fb *= k; ft *= k;
            }
            m_fL[c] = fl; m_fR[c] = fr; m_fB[c] = fb; m_fT[c] = ft;
        }

    // 3. Apply net volume change: inflow (neighbour's flux toward me) − outflow.
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            int c = idx(i,j);
            float inflow = 0.0f;
            if (i > 0)   inflow += m_fR[idx(i-1,j)]; // left neighbour's right pipe
            if (i < n-1) inflow += m_fL[idx(i+1,j)];
            if (j > 0)   inflow += m_fT[idx(i,j-1)];
            if (j < n-1) inflow += m_fB[idx(i,j+1)];
            float outflow = m_fL[c] + m_fR[c] + m_fB[c] + m_fT[c];
            float dV = (inflow - outflow) * dt;
            m_water[c] = std::max(0.0f, m_water[c] + dV / dx2);
        }
}

void ShallowWaterSim::collectOverflowEdges(float dropThresh, std::vector<Overflow>& out) const {
    out.clear();
    const int dx[4] = { -1, 1, 0, 0 };
    const int dy[4] = { 0, 0, -1, 1 };
    for (int j = 0; j < m_n; ++j) {
        for (int i = 0; i < m_n; ++i) {
            int c = idx(i, j);
            float w = m_water[c];
            if (w < 0.05f) continue;                 // needs real water to spill
            float surf = m_terrain[c] + w;           // water surface height here
            // Find the neighbour with the biggest surface drop (steepest spill).
            int   bestN = -1; float bestDrop = dropThresh;
            for (int d = 0; d < 4; ++d) {
                int ni = i + dx[d], nj = j + dy[d];
                if (ni < 0 || nj < 0 || ni >= m_n || nj >= m_n) continue;
                int nc = idx(ni, nj);
                float nsurf = m_terrain[nc] + m_water[nc];
                float drop = surf - nsurf;
                if (drop > bestDrop) { bestDrop = drop; bestN = d; }
            }
            if (bestN < 0) continue;                 // no steep edge → not a waterfall
            Overflow o;
            o.worldPos = worldPosAt(i, j) + m_up * double(m_terrain[c] + w);
            glm::dvec3 nd = m_tan * double(dx[bestN]) + m_bit * double(dy[bestN]);
            double nl = glm::length(nd);
            o.flowDir  = (nl > 1e-9) ? nd / nl : m_tan;
            o.drop     = bestDrop;
            // Spill rate proxy: water depth × cell area / second (bounded).
            o.rate     = glm::clamp(w, 0.0f, 2.0f) * float(m_dx * m_dx);
            out.push_back(o);
        }
    }
}

} // namespace Haruka::fluid
