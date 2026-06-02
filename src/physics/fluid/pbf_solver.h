/**
 * @file pbf_solver.h
 * @brief Position-Based Fluids (Fase C): local 3D splashy fluid on the XPBD
 *        position-projection model.
 *
 * PBF (Macklin & Müller, "Position Based Fluids", 2013) is XPBD with a per-
 * particle DENSITY constraint: each particle is pushed so the SPH-estimated
 * density equals a rest density. Pressure-like corrections + XSPH viscosity give
 * a fluid that pours, splashes and settles — the same predict→project→update
 * loop as the softbody solver, so it shares the engine's conventions.
 *
 * Neighbour search uses a uniform spatial hash sized to the smoothing radius h.
 * Positions are camera/origin-relative (small floats) → planet-safe.
 *
 * CPU-first with flat SoA arrays so it can move to a GPU compute pass unchanged.
 */
#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <functional>
#include <unordered_map>

namespace Haruka::fluid {

class PBFSolver {
public:
    // --- Tunables ---
    float h          = 0.6f;    ///< smoothing radius (m); particle spacing ~0.5h
    float restDensity= 1000.0f; ///< target density
    float relaxation = 100.0f;  ///< CFM softening (ε); larger = more compliant/stable
    float viscosity  = 0.01f;   ///< XSPH viscosity coefficient
    int   solverIters= 3;       ///< density-projection iterations per step
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};

    /** @brief Planet-aware gravity (world pos → accel). Overrides `gravity`. */
    std::function<glm::vec3(const glm::dvec3&)> gravityProvider;

    /** @brief Surface collision: world pos → surface point + outward normal. */
    std::function<bool(const glm::dvec3&, glm::dvec3&, glm::dvec3&)> surfaceQuery;

    /** @brief Local frame origin; world = origin + position[i]. */
    glm::dvec3 origin{0.0};

    // --- SoA particle state (local frame) ---
    std::vector<glm::vec3> position;
    std::vector<glm::vec3> velocity;

    /** @brief Adds a particle at a local position. */
    int add(const glm::vec3& p) {
        position.push_back(p);
        velocity.push_back(glm::vec3(0.0f));
        return (int)position.size() - 1;
    }
    int count() const { return (int)position.size(); }
    glm::dvec3 worldPos(int i) const { return origin + glm::dvec3(position[i]); }
    void clear() { position.clear(); velocity.clear(); }

    /** @brief Per-particle mass, so volume transfers (Fase E) conserve mass.
     *  Chosen so a rest-spaced lattice reads ~restDensity. */
    float particleMass() const { return restDensity * (h*0.5f) * (h*0.5f) * (h*0.5f); }

    /** @brief Volume (m³) carried by one particle. */
    float particleVolume() const { return particleMass() / restDensity; }

    /** @brief Removes a particle by swapping with the last (O(1)). */
    void removeParticle(int i) {
        int last = (int)position.size() - 1;
        if (i < 0 || i > last) return;
        position[i] = position[last]; velocity[i] = velocity[last];
        position.pop_back(); velocity.pop_back();
    }

    /** @brief Advances the fluid by dt seconds. */
    void step(float dt);

private:
    // Scratch (kept across frames to avoid reallocation).
    std::vector<glm::vec3> m_pred;     // predicted positions
    std::vector<float>     m_lambda;   // per-particle constraint multiplier
    std::vector<glm::vec3> m_dp;       // position deltas
    std::vector<std::vector<int>> m_neighbors;

    // Spatial hash.
    std::unordered_map<long long, std::vector<int>> m_grid;
    long long cellKey(const glm::vec3& p) const;
    void buildGrid();
    void findNeighbors();
    void collide(glm::vec3& p, glm::vec3& v) const;
};

} // namespace Haruka::fluid
