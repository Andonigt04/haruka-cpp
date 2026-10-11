/**
 * @file Constraints.h
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

#include "ParticleSystem.h"
#include <glm/glm.hpp>
#include <vector>

namespace Haruka::Physics::XPBD {

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
    /// Sólo TIRA (una tela, un tendón): estirada devuelve, comprimida no empuja — se comba. Bilateral,
    /// una membrana plegada (un ala de murciélago cerrándose) empujaba a los huesos a los que va cosida
    /// con decenas de g (medido: 18-58 g en una falange de 875 kg con el ala plegada).
    bool  stretchOnly = false;
    /// TOPE DE ESTIRAMIENTO (fracción del largo; 0 = sin tope). Dentro del tope la arista es elástica
    /// (su `compliance`); en el tope es RÍGIDA: nunca pasa de restLength·(1+maxStretch). Es el
    /// "strain limiting" de las telas: absorbe tirones sin la rigidez infinita de una arista a 0, y no
    /// deja que la tela se deforme de cualquier manera (la piel llegaba a separarse 13 m: el espagueti).
    float maxStretch = 0.0f;

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
        // Tensión = λ ≤ 0 (C > 0 da Δλ < 0). Sólo tirando: λ acumulado nunca pasa a compresión.
        if (stretchOnly && lambda + dLambda > 0.0f) dLambda = -lambda;
        lambda += dLambda;

        glm::vec3 corr = dLambda * n;
        ps.position[a] += wa * corr;
        ps.position[b] -= wb * corr;

        // El tope, después y DURO (proyección PBD, sin compliancia): si la parte elástica no bastó.
        if (maxStretch > 0.0f) {
            const glm::vec3 d2 = ps.position[a] - ps.position[b];
            const float len2 = glm::length(d2);
            const float lim = restLength * (1.0f + maxStretch);
            if (len2 > lim && len2 > 1e-8f) {
                const glm::vec3 c2 = ((len2 - lim) / wSum) * (d2 / len2);
                ps.position[a] -= wa * c2;
                ps.position[b] += wb * c2;
            }
        }
    }
};

/**
 * @brief ENGANCHE: tira de una partícula hacia un punto que mueve el JUEGO (un hueso).
 *
 * Es lo que hace que una piel o una membrana SIGA a un cuerpo rígido sin ser rígida: con
 * `compliance` > 0 la partícula llega tarde y se pasa (inercia), y eso es lo que se ve moverse. El
 * objetivo lo actualiza quien lleva el hueso antes de cada `step`, en el MARCO LOCAL del sistema.
 *
 * `force` es la fuerza que la partícula hace SOBRE EL HUESO en el último subpaso (N, marco local):
 * la reacción del enganche. Sumada por hueso, es lo que devuelve el peso de la piel y el tirón de una
 * membrana al cuerpo rígido — sin ella la piel sería decorado.
 */
struct AttachmentConstraint {
    int       p = 0;
    glm::vec3 target{0.0f};
    /// Velocidad del punto del hueso (m/s). La usa el amortiguamiento RELATIVO del solver
    /// (`XPBDSolver::attachDampingRate`): se frena lo que la partícula se mueve RESPECTO a su hueso,
    /// no respecto al mundo — si no, una piel que vuela a 50 m/s va siempre frenada y rezagada.
    glm::vec3 targetVel{0.0f};
    /// Objetivo de ESTE subpaso: `target` es donde está el hueso al FINAL del frame, y dentro del frame
    /// el hueso se mueve. Con el objetivo quieto en su punto final durante los 8 subpasos, una piel que
    /// va a 50 m/s llegaba antes de tiempo y se rezagaba después (medido: 0,36 m). `XPBDSolver::step`
    /// lo avanza con `targetVel`.
    glm::vec3 subTarget{0.0f};
    float     compliance = 0.0f;
    /// Amortiguamiento RELATIVO al hueso de ESTE enganche (1/s); < 0 = el del solver
    /// (`XPBDSolver::attachDampingRate`). Hace falta por partícula cuando los enganches tienen
    /// frecuencias distintas: con una tasa común, ζ = tasa/(2ω) sale bajo en los rígidos (se pasan) y
    /// muy alto en los blandos (se arrastran sin rebotar), justo al revés de lo que se quiere.
    float     dampingRate = -1.0f;
    float     lambda = 0.0f;
    /// Reacción sobre el hueso, MEDIA de todos los subpasos del último `step` (N). Ver `project`.
    glm::vec3 force{0.0f};
    void project(ParticleSystem& ps, float dt2) {
        const float w = ps.inverseMass[p];
        if (w <= 0.0f) return;
        glm::vec3 d = ps.position[p] - subTarget;
        const float len = glm::length(d);
        if (len < 1e-7f) return;
        const glm::vec3 n = d / len;
        const float alphaTilde = compliance / dt2;
        const float dLambda = (-len - alphaTilde * lambda) / (w + alphaTilde);
        lambda += dLambda;
        ps.position[p] += w * dLambda * n;
        // Fuerza del enganche sobre la partícula = λ·∇C/h² (hacia el objetivo); sobre el hueso, la
        // opuesta. λ < 0 aquí, así que -λ·n/h² apunta del hueso a la partícula: la reacción.
        force += -(lambda / dt2) * n;   // se acumula; `XPBDSolver::step` lo divide entre los subpasos
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

} // namespace Haruka::Physics::XPBD
