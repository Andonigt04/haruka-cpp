#include "pbf_solver.h"
#include <algorithm>
#include <cmath>

namespace Haruka::fluid {

// --- SPH kernels (Müller et al. poly6 / spiky), 3D ---
static inline float poly6(float r2, float h) {
    float h2 = h * h;
    if (r2 >= h2) return 0.0f;
    float t = h2 - r2;
    float c = 315.0f / (64.0f * 3.14159265f * std::pow(h, 9));
    return c * t * t * t;
}
static inline glm::vec3 spikyGrad(const glm::vec3& rij, float h) {
    float r = glm::length(rij);
    if (r <= 1e-6f || r >= h) return glm::vec3(0.0f);
    float c = -45.0f / (3.14159265f * std::pow(h, 6));
    float t = (h - r);
    return (c * t * t / r) * rij;
}

long long PBFSolver::cellKey(const glm::vec3& p) const {
    // Hash a grid cell of side h.
    long long xi = (long long)std::floor(p.x / h);
    long long yi = (long long)std::floor(p.y / h);
    long long zi = (long long)std::floor(p.z / h);
    // Cantor-ish combine into 64 bits (good enough for sparse fluids).
    const long long P1 = 73856093, P2 = 19349663, P3 = 83492791;
    return (xi * P1) ^ (yi * P2) ^ (zi * P3);
}

void PBFSolver::buildGrid() {
    m_grid.clear();
    m_grid.reserve(m_pred.size() * 2);
    for (int i = 0; i < (int)m_pred.size(); ++i)
        m_grid[cellKey(m_pred[i])].push_back(i);
}

void PBFSolver::findNeighbors() {
    const int n = (int)m_pred.size();
    m_neighbors.assign(n, {});
    const float h2 = h * h;
    for (int i = 0; i < n; ++i) {
        const glm::vec3& pi = m_pred[i];
        long long bx = (long long)std::floor(pi.x / h);
        long long by = (long long)std::floor(pi.y / h);
        long long bz = (long long)std::floor(pi.z / h);
        const long long P1 = 73856093, P2 = 19349663, P3 = 83492791;
        for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            long long key = ((bx+dx)*P1) ^ ((by+dy)*P2) ^ ((bz+dz)*P3);
            auto it = m_grid.find(key);
            if (it == m_grid.end()) continue;
            for (int j : it->second) {
                if (j == i) continue;
                glm::vec3 rij = pi - m_pred[j];
                if (glm::dot(rij, rij) < h2) m_neighbors[i].push_back(j);
            }
        }
    }
}

void PBFSolver::collide(glm::vec3& p, glm::vec3& v) const {
    if (!surfaceQuery) return;
    glm::dvec3 wp = origin + glm::dvec3(p);
    glm::dvec3 surfW, nrm;
    if (!surfaceQuery(wp, surfW, nrm)) return;
    double sd = glm::dot(wp - surfW, nrm);
    if (sd < 0.0) {
        p += glm::vec3(nrm * (-sd));
        glm::vec3 ln = glm::vec3(nrm);
        float vn = glm::dot(v, ln);
        if (vn < 0.0f) v -= vn * ln; // remove into-surface velocity
    }
}

void PBFSolver::step(float dt) {
    const int n = count();
    if (n == 0 || dt <= 0.0f) return;
    dt = std::min(dt, 1.0f / 30.0f);

    m_pred.resize(n);
    m_lambda.assign(n, 0.0f);
    m_dp.assign(n, glm::vec3(0.0f));

    // 1. Predict positions (gravity + inertia).
    for (int i = 0; i < n; ++i) {
        glm::vec3 g = gravity;
        if (gravityProvider) g = gravityProvider(worldPos(i));
        velocity[i] += g * dt;
        m_pred[i] = position[i] + velocity[i] * dt;
    }

    // 2. Neighbour search on predicted positions.
    buildGrid();
    findNeighbors();

    const float restInv = 1.0f / restDensity;
    // mass chosen so a rest-spaced lattice reads ~restDensity (matches particleMass()).
    const float mass = particleMass();

    // 3. Density-constraint iterations.
    for (int iter = 0; iter < solverIters; ++iter) {
        // 3a. Compute lambda per particle.
        for (int i = 0; i < n; ++i) {
            float rho = poly6(0.0f, h) * mass; // self
            for (int j : m_neighbors[i]) {
                glm::vec3 rij = m_pred[i] - m_pred[j];
                rho += mass * poly6(glm::dot(rij, rij), h);
            }
            float C = rho * restInv - 1.0f;

            // Sum of squared gradients.
            glm::vec3 gradI(0.0f);
            float sumGrad2 = 0.0f;
            for (int j : m_neighbors[i]) {
                glm::vec3 grad = spikyGrad(m_pred[i] - m_pred[j], h) * (mass * restInv);
                gradI += grad;
                sumGrad2 += glm::dot(grad, grad);
            }
            sumGrad2 += glm::dot(gradI, gradI);
            m_lambda[i] = -C / (sumGrad2 + relaxation);
        }

        // 3b. Position deltas (symmetric pressure + surface tension corr).
        for (int i = 0; i < n; ++i) {
            glm::vec3 dp(0.0f);
            for (int j : m_neighbors[i]) {
                // sCorr: artificial pressure to avoid clustering (Δq = 0.2h).
                glm::vec3 rij = m_pred[i] - m_pred[j];
                float w = poly6(glm::dot(rij, rij), h);
                float wq = poly6((0.2f*h)*(0.2f*h), h);
                float sCorr = (wq > 1e-12f) ? -0.001f * std::pow(w / wq, 4.0f) : 0.0f;
                dp += (m_lambda[i] + m_lambda[j] + sCorr) * spikyGrad(rij, h) * (mass * restInv);
            }
            m_dp[i] = dp;
        }
        // 3c. Apply deltas + collide.
        for (int i = 0; i < n; ++i) {
            m_pred[i] += m_dp[i];
            glm::vec3 vtmp = velocity[i];
            collide(m_pred[i], vtmp);
        }
    }

    // 4. Update velocity from position change, then XSPH viscosity.
    // Clamp speed to a multiple of the cell-crossing rate: without confinement,
    // edge particles get asymmetric pressure and can otherwise gain runaway
    // velocity (instability / NaN). This keeps the sim stable in open scenes.
    const float invDt = 1.0f / dt;
    const float vMax = 3.0f * h * invDt; // ≤3 smoothing radii per step
    for (int i = 0; i < n; ++i) {
        glm::vec3 v = (m_pred[i] - position[i]) * invDt;
        float sp = glm::length(v);
        if (sp > vMax) v *= (vMax / sp);
        if (!(sp == sp)) v = glm::vec3(0.0f); // NaN guard
        velocity[i] = v;
    }

    if (viscosity > 0.0f) {
        std::vector<glm::vec3> vnew(n);
        for (int i = 0; i < n; ++i) {
            glm::vec3 acc(0.0f);
            for (int j : m_neighbors[i]) {
                glm::vec3 rij = m_pred[i] - m_pred[j];
                acc += (velocity[j] - velocity[i]) * poly6(glm::dot(rij, rij), h);
            }
            vnew[i] = velocity[i] + viscosity * acc;
        }
        velocity.swap(vnew);
    }

    // 5. Commit positions + final collision.
    for (int i = 0; i < n; ++i) {
        position[i] = m_pred[i];
        collide(position[i], velocity[i]);
    }
}

} // namespace Haruka::fluid
