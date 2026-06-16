/**
 * @file particle_system.h
 * @brief Structure-of-Arrays particle pool for the XPBD solver (Fase A).
 *
 * Particle positions are stored in a LOCAL frame relative to m_origin (double),
 * exactly like the terrain/ocean: world = m_origin + vec3(localPos). This keeps
 * per-particle math in float precision even at planetary distances, and lets the
 * whole pool ride along on a floating-origin shift.
 *
 * SoA layout is deliberate: it keeps the hot solver loops cache-friendly and is
 * the same memory shape we'd upload to a GPU compute buffer later (Fase C/F),
 * so moving the solver to the GPU won't require a data redesign.
 */
#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

namespace Haruka::xpbd {

/** @brief Phase/group tag so collisions can be filtered (self-collision off, etc.). */
using Phase = uint32_t;

/**
 * @brief SoA pool of simulation particles.
 *
 * inverseMass == 0 marks a pinned/kinematic particle (infinite mass).
 */
class ParticleSystem {
public:
    // --- Hot SoA arrays (parallel; same index = same particle) ---
    std::vector<glm::vec3> position;     ///< current local position (cam/origin-relative)
    std::vector<glm::vec3> prevPosition; ///< position at start of substep (XPBD)
    std::vector<glm::vec3> velocity;     ///< local-frame velocity
    std::vector<float>     inverseMass;  ///< 0 = pinned (infinite mass)
    std::vector<Phase>     phase;        ///< group tag

    /** @brief Origin of the local frame; world = origin + position. */
    glm::dvec3 origin{0.0};

    /** @brief Adds a particle, returns its index. mass<=0 ⇒ pinned. */
    int add(const glm::vec3& localPos, float mass, Phase ph = 0) {
        position.push_back(localPos);
        prevPosition.push_back(localPos);
        velocity.push_back(glm::vec3(0.0f));
        inverseMass.push_back(mass > 0.0f ? 1.0f / mass : 0.0f);
        phase.push_back(ph);
        return static_cast<int>(position.size()) - 1;
    }

    /** @brief Number of particles. */
    int count() const { return static_cast<int>(position.size()); }

    /** @brief World-space position of a particle (double). */
    glm::dvec3 worldPos(int i) const { return origin + glm::dvec3(position[i]); }

    /** @brief Removes all particles (keeps origin). */
    void clear() {
        position.clear(); prevPosition.clear(); velocity.clear();
        inverseMass.clear(); phase.clear();
    }

    /**
     * @brief Rebases the local frame to a new origin, shifting all positions so
     * world positions are unchanged. Call on a floating-origin shift.
     */
    void rebase(const glm::dvec3& newOrigin) {
        glm::vec3 delta = glm::vec3(origin - newOrigin);
        for (auto& p : position)     p += delta;
        for (auto& p : prevPosition) p += delta;
        origin = newOrigin;
    }
};

} // namespace Haruka::xpbd
