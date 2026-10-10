#include "XPBDSolver.h"
#include <algorithm>
#include <cmath>

namespace Haruka::Physics::XPBD {

void XPBDSolver::step(float dt) {
    if (dt <= 0.0f || particles.count() == 0) return;
    // Clamp dt so a frame hitch can't explode the sim.
    dt = std::min(dt, 1.0f / 30.0f);

    const int   n = std::max(1, substeps);
    const float h = dt / float(n);
    const float invH = 1.0f / h;
    const float h2 = h * h;

    for (auto& c : attachments) c.force = glm::vec3(0.0f);
    for (int s = 0; s < n; ++s) {
        if (preSubstep) preSubstep(*this, h);
        // El hueso, donde está en ESTE subpaso: del final del frame hacia atrás con su velocidad.
        const float back = float(n - 1 - s) * h;
        for (auto& c : attachments) c.subTarget = c.target - c.targetVel * back;
        predict(h);
        // Reset Lagrange multipliers each substep (XPBD).
        for (auto& c : distanceConstraints) c.lambda = 0.0f;
        for (auto& c : volumeConstraints)   c.lambda = 0.0f;
        for (auto& c : attachments)         c.lambda = 0.0f;
        solveConstraints(h2);
        if (postSolve) postSolve(*this, s);
        collide();
        updateVelocity(invH);
        if (attachDampingRate > 0.0f) dampAttached(h);
    }
    // La reacción de cada enganche, MEDIA del paso: la del último subpaso sola es ruido.
    for (auto& c : attachments) c.force /= float(n);
}

void XPBDSolver::dampAttached(float h) {
    const int np = particles.count();
    if ((int)m_attachOf.size() != np) {
        m_attachOf.assign(np, -1);
        for (size_t i = 0; i < attachments.size(); ++i)
            if (attachments[i].p >= 0 && attachments[i].p < np) m_attachOf[attachments[i].p] = (int)i;
    }
    const float k = std::exp(-attachDampingRate * h);
    for (int i = 0; i < np; ++i) {
        const int a = m_attachOf[i];
        if (a < 0 || particles.inverseMass[i] == 0.0f) continue;
        const glm::vec3 tv = attachments[a].targetVel;
        particles.velocity[i] = tv + (particles.velocity[i] - tv) * k;
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

        glm::vec3 a = g + w;
        if (i < (int)externalForce.size()) a += externalForce[i] * particles.inverseMass[i];
        particles.velocity[i] += a * h;
        particles.prevPosition[i] = particles.position[i];
        particles.position[i]    += particles.velocity[i] * h;
    }
}

void XPBDSolver::solveConstraints(float h2) {
    // One Gauss-Seidel pass per substep (substepping replaces iteration count).
    for (auto& c : distanceConstraints) c.project(particles, h2);
    for (auto& c : volumeConstraints)   c.project(particles, h2);
    for (auto& c : attachments)         c.project(particles, h2);
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

} // namespace Haruka::Physics::XPBD
