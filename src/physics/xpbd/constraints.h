/**
 * @file constraints.h
 * @brief XPBD constraints. Fase A ships the distance constraint (ropes/cloth edges).
 *
 * XPBD solves each constraint by projecting particle positions to satisfy
 * C(x)=0, with a per-constraint compliance α (inverse stiffness, in m²/N) that
 * makes stiffness independent of iteration count and timestep — the key XPBD
 * property over classic PBD.
 *
 * Reference: Müller et al., "XPBD: Position-Based Simulation of Compliant
 * Constrained Dynamics".
 */
#pragma once

#include "particle_system.h"
#include <glm/glm.hpp>
#include <vector>

namespace Haruka::xpbd {

/**
 * @brief Distance constraint between two particles.
 *
 * compliance 0 ⇒ perfectly rigid; larger ⇒ softer/stretchy.
 * lambda is the running Lagrange multiplier, reset each substep.
 */
struct DistanceConstraint {
    int   a = 0, b = 0;
    float restLength = 0.0f;
    float compliance = 0.0f; // α (m²/N); 0 = rigid
    float lambda = 0.0f;     // accumulated multiplier (per substep)

    /** @brief Projects a/b to satisfy the rest-length, scaled by dt² and masses. */
    void project(ParticleSystem& ps, float dt2) {
        const float wa = ps.inverseMass[a];
        const float wb = ps.inverseMass[b];
        const float wSum = wa + wb;
        if (wSum <= 0.0f) return; // both pinned

        glm::vec3 d = ps.position[a] - ps.position[b];
        float len = glm::length(d);
        if (len < 1e-8f) return;
        glm::vec3 n = d / len;

        float C = len - restLength;
        // XPBD: Δλ = (-C - α̃·λ) / (wSum + α̃), with α̃ = α/dt²
        float alphaTilde = compliance / dt2;
        float dLambda = (-C - alphaTilde * lambda) / (wSum + alphaTilde);
        lambda += dLambda;

        glm::vec3 corr = dLambda * n;
        ps.position[a] += wa * corr;
        ps.position[b] -= wb * corr;
    }
};

/**
 * @brief Volume-preservation constraint over a tetrahedron (a,b,c,d).
 *
 * Keeps the signed volume of the tet at its rest value, so a filled mesh of tets
 * behaves like an incompressible soft solid (jelly) instead of collapsing under
 * the edge (distance) constraints alone.
 *
 * C = 6·(V - restVolume); gradients are the standard cross-product form.
 */
struct VolumeConstraint {
    int   a = 0, b = 0, c = 0, d = 0;
    float restVolume = 0.0f; // 6× actual rest volume (kept consistent with C)
    float compliance = 0.0f;
    float lambda = 0.0f;

    static float sixVolume(const glm::vec3& p0, const glm::vec3& p1,
                           const glm::vec3& p2, const glm::vec3& p3) {
        return glm::dot(glm::cross(p1 - p0, p2 - p0), p3 - p0);
    }

    void project(ParticleSystem& ps, float dt2) {
        glm::vec3& pa = ps.position[a];
        glm::vec3& pb = ps.position[b];
        glm::vec3& pc = ps.position[c];
        glm::vec3& pd = ps.position[d];

        // Gradients (∇C) per vertex for the 6V form.
        glm::vec3 ga = glm::cross(pd - pb, pc - pb);
        glm::vec3 gb = glm::cross(pc - pa, pd - pa);
        glm::vec3 gc = glm::cross(pd - pa, pb - pa);
        glm::vec3 gd = glm::cross(pb - pa, pc - pa);

        const float wa = ps.inverseMass[a], wb = ps.inverseMass[b];
        const float wc = ps.inverseMass[c], wd = ps.inverseMass[d];
        float denom = wa*glm::dot(ga,ga) + wb*glm::dot(gb,gb)
                    + wc*glm::dot(gc,gc) + wd*glm::dot(gd,gd);
        if (denom < 1e-12f) return;

        float C = sixVolume(pa, pb, pc, pd) - restVolume;
        float alphaTilde = compliance / dt2;
        float dLambda = (-C - alphaTilde * lambda) / (denom + alphaTilde);
        lambda += dLambda;

        pa += wa * dLambda * ga;
        pb += wb * dLambda * gb;
        pc += wc * dLambda * gc;
        pd += wd * dLambda * gd;
    }
};

} // namespace Haruka::xpbd
