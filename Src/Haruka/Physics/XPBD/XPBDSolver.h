/**
 * @file XPBDSolver.h
 * @brief Substepping XPBD solver (Fase A): the shared base for softbody (Fase B)
 *        and position-based fluids (Fase C).
 *
 * Pipeline per frame:
 *   for each substep:
 *     predict()         — integrate gravity + inertia into a predicted position
 *     solveConstraints()— project distance constraints (compliance α)
 *     collide()         — push particles out of the planet surface
 *     updateVelocity()  — v = (x - xPrev) / dt
 *
 * Substepping (small dt, 1 constraint iteration each) gives stable stiff
 * behaviour without large iteration counts. Reference: Müller, "Detailed Rigid
 * Body Simulation with XPBD" (2020).
 *
 * Runs in the particle's local frame; collision queries convert to world via
 * ParticleSystem::worldPos and the supplied terrain-height callback, so it works
 * unchanged at planetary scale.
 */
#pragma once

#include "ParticleSystem.h"
#include "Constraints.h"
#include <glm/glm.hpp>
#include <functional>
#include <vector>

namespace Haruka::xpbd {

class XPBDSolver {
public:
    /** @brief Local-frame gravity (m/s²). For a planet, point it toward the core
     *  via setGravityProvider; the constant is a fallback. */
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    int       substeps = 8;

    /**
     * @brief Per-particle gravity direction provider (planet-aware).
     * Given a world position, returns the gravity acceleration vector (m/s²).
     * If unset, the constant `gravity` is used.
     */
    std::function<glm::vec3(const glm::dvec3& worldPos)> gravityProvider;

    /**
     * @brief Terrain collision callback: returns the world-space surface height
     * offset (metres above the reference sphere) and outward normal at a world
     * position. Return false if no planet/terrain applies there.
     */
    std::function<bool(const glm::dvec3& worldPos,
                       glm::dvec3& outSurfaceWorld,
                       glm::dvec3& outNormal)> surfaceQuery;

    ParticleSystem particles;
    std::vector<DistanceConstraint> distanceConstraints;
    std::vector<VolumeConstraint>   volumeConstraints;
    std::vector<AttachmentConstraint> attachments;   ///< partículas que siguen a un hueso (ver Constraints.h)
    /// Amortiguamiento de la velocidad RELATIVA al hueso de las partículas enganchadas (1/s): tras cada
    /// subpaso, `v = vHueso + (v − vHueso)·e^(−tasa·h)`. Es lo que hace que una piel se ASIENTE sin
    /// frenar a la criatura entera (el `damping` de arriba frena respecto al mundo).
    float attachDampingRate = 0.0f;
    /// Fuerza externa por partícula (N, marco local), aplicada en cada subpaso. Vacío = ninguna. La
    /// rellena quien la calcule — el aire sobre una membrana — normalmente desde `preSubstep`.
    std::vector<glm::vec3> externalForce;
    /// Se llama al EMPEZAR cada subpaso con su `h`: para fuerzas que dependen del estado y cambian
    /// deprisa (el aire sobre una tela ligera; calculada una vez por frame oscilaría).
    std::function<void(XPBDSolver&, float h)> preSubstep;
    /// Se llama tras resolver las restricciones de cada subpaso, con su índice: para colisiones propias
    /// (la piel de una criatura contra los volúmenes de sus huesos). Quien la ponga decide cada cuántos
    /// subpasos trabaja: con cientos de volúmenes, cada subpaso sería caro.
    std::function<void(XPBDSolver&, int substep)> postSolve;

    /** @brief Velocity damping per second [0..1]; bleeds energy so cloth/jelly settle. */
    float damping = 0.01f;

    /**
     * @brief Localized wind provider (world pos → acceleration m/s²). Unset or
     * returning zero means no wind at that point — so a flag only billows when it
     * is INSIDE a wind zone. Drive this from a WindField (Physics/WindField.h).
     */
    std::function<glm::vec3(const glm::dvec3& worldPos)> windQuery;

    /** @brief Advances the simulation by dt seconds. */
    void step(float dt);
    /** @brief Mueve el origen del marco local sin mover nada en el mundo: partículas Y objetivos de
     *  los enganches (con sólo `particles.rebase` los enganches tirarían hacia el sitio viejo). */
    void rebase(const glm::dvec3& newOrigin) {
        const glm::vec3 delta = glm::vec3(particles.origin - newOrigin);
        for (auto& a : attachments) a.target += delta;
        particles.rebase(newOrigin);
    }

private:
    void predict(float h);
    void solveConstraints(float h);
    void collide();
    void updateVelocity(float invH);
    void dampAttached(float h);
    std::vector<int> m_attachOf;   ///< partícula → índice de enganche (−1 = libre); se rehace si cambia
};

} // namespace Haruka::xpbd
