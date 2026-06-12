#include "shallow_water.h"
#include <algorithm>
#include <cmath>

namespace Haruka::fluid {

void ShallowWaterSim::init(const glm::dvec3& anchor,
                           const glm::dvec3& tangent,
                           const glm::dvec3& bitangent,
                           const glm::dvec3& up,
                           double spanM, int n,
                           std::function<double(const glm::dvec3&)> terrainHeight) {
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

void ShallowWaterSim::applySeaLevel(float seaLevelAlongUp) {
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
