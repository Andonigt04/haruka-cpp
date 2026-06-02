#include "xpbd_solver.h"
#include <algorithm>
#include <cmath>

namespace Haruka::xpbd {

void XPBDSolver::step(float dt) {
    if (dt <= 0.0f || particles.count() == 0) return;
    // Clamp dt so a frame hitch can't explode the sim.
    dt = std::min(dt, 1.0f / 30.0f);

    const int   n = std::max(1, substeps);
    const float h = dt / float(n);
    const float invH = 1.0f / h;
    const float h2 = h * h;

    for (int s = 0; s < n; ++s) {
        predict(h);
        // Reset Lagrange multipliers each substep (XPBD).
        for (auto& c : distanceConstraints) c.lambda = 0.0f;
        for (auto& c : volumeConstraints)   c.lambda = 0.0f;
        solveConstraints(h2);
        collide();
        updateVelocity(invH);
    }
}

void XPBDSolver::predict(float h) {
    const int n = particles.count();
    for (int i = 0; i < n; ++i) {
        if (particles.inverseMass[i] == 0.0f) {
            particles.prevPosition[i] = particles.position[i];
            continue;
        }
        glm::vec3 g = gravity;
        if (gravityProvider) g = gravityProvider(particles.worldPos(i));

        // Localized wind: zero unless this particle is inside a wind zone, so a
        // flag only billows where wind actually blows.
        glm::vec3 w(0.0f);
        if (windQuery) w = windQuery(particles.worldPos(i));

        particles.velocity[i] += (g + w) * h;
        particles.prevPosition[i] = particles.position[i];
        particles.position[i]    += particles.velocity[i] * h;
    }
}

void XPBDSolver::solveConstraints(float h2) {
    // One Gauss-Seidel pass per substep (substepping replaces iteration count).
    for (auto& c : distanceConstraints) c.project(particles, h2);
    for (auto& c : volumeConstraints)   c.project(particles, h2);
}

void XPBDSolver::collide() {
    if (!surfaceQuery) return;
    const int n = particles.count();
    for (int i = 0; i < n; ++i) {
        if (particles.inverseMass[i] == 0.0f) continue;
        glm::dvec3 wp = particles.worldPos(i);
        glm::dvec3 surfW, nrm;
        if (!surfaceQuery(wp, surfW, nrm)) continue;

        // Signed distance above the surface along the outward normal.
        double sd = glm::dot(wp - surfW, nrm);
        if (sd < 0.0) {
            // Push back onto the surface (local frame).
            glm::vec3 corr = glm::vec3(nrm * (-sd));
            particles.position[i] += corr;
            // Kill into-surface velocity component (simple friction-free bounce-less).
            glm::vec3 ln = glm::vec3(nrm);
            float vn = glm::dot(particles.velocity[i], ln);
            if (vn < 0.0f) particles.velocity[i] -= vn * ln;
        }
    }
}

void XPBDSolver::updateVelocity(float invH) {
    const int n = particles.count();
    const float damp = 1.0f - glm::clamp(damping, 0.0f, 1.0f);
    for (int i = 0; i < n; ++i) {
        if (particles.inverseMass[i] == 0.0f) { particles.velocity[i] = glm::vec3(0.0f); continue; }
        particles.velocity[i] = (particles.position[i] - particles.prevPosition[i]) * invH * damp;
    }
}

} // namespace Haruka::xpbd
