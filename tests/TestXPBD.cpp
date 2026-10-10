// ================================================================================================
// Tests de XPBD: el ENGANCHE (una partícula que sigue a un hueso), que es lo que lleva la piel y las
// membranas de las criaturas. Tres cosas que se vieron mal en el juego y aquí se miden, cada una con
// su contraprueba: la piel no se asentaba nunca, iba rezagada al volar, y su peso tiene que llegar al
// hueso.
// ================================================================================================
#include "TestCommon.h"
#include "Haruka/Core/Modules.h"

#include <cmath>
#include <glm/glm.hpp>

#ifdef HARUKA_MOD_SOFTBODY
#include "Haruka/Physics/XPBD/XPBDSolver.h"

namespace {
using Haruka::Physics::XPBD::XPBDSolver;
using Haruka::Physics::XPBD::AttachmentConstraint;

/** @brief Una partícula de 10 kg enganchada a 4 Hz a un objetivo en el origen. */
void oneAttached(XPBDSolver& s, float hz) {
    const float m = 10.0f;
    s.gravity = glm::vec3(0.0f);
    s.damping = 0.0f;
    s.particles.add(glm::vec3(0.0f), m);
    AttachmentConstraint a;
    a.p = 0;
    const float w = 2.0f * 3.14159265f * hz;
    a.compliance = 1.0f / (m * w * w);
    s.attachments.push_back(a);
}
} // namespace

void test_xpbd_attachment() {
    beginTest("xpbd_attachment");
    const float dt = 1.0f / 60.0f;

    // (1) SE ASIENTA: el hueso da un salto de 1 m y la piel lo sigue. Con amortiguamiento relativo, a
    //     los 2,5-3 s ya no se mueve; sin él, oscila para siempre con el mismo metro de amplitud.
    auto settle = [&](float rate) {
        XPBDSolver s; oneAttached(s, 4.0f);
        s.attachDampingRate = rate;
        s.attachments[0].target = glm::vec3(1.0f, 0.0f, 0.0f);
        double worst = 0.0;
        for (int i = 0; i < 180; ++i) {
            s.step(dt);
            if (i >= 150) worst = std::max(worst, (double)glm::length(s.particles.position[0] - s.attachments[0].target));
        }
        return worst;
    };
    const double sDamped = settle(6.0f), sFree = settle(0.0f);
    std::printf("    salto de 1 m: separacion en 2,5-3 s = %.4f m amortiguado · %.3f m sin amortiguar\n",
                sDamped, sFree);
    CHECK(sDamped < 0.02, "con amortiguamiento RELATIVO la piel se asienta tras un tiron");
    CHECK(sFree > 0.1, "CONTRAPRUEBA: sin amortiguar sigue oscilando (XPBD ya pierde algo solo)");

    // (2) SIGUE sin rezagarse: el hueso va a 50 m/s. Amortiguar respecto al HUESO no frena nada que
    //     vaya con él; amortiguar respecto al MUNDO con la misma tasa es un freno de aire constante y
    //     la piel se queda atrás (rezago = tasa·v/ω²).
    auto follow = [&](bool relative) {
        XPBDSolver s; oneAttached(s, 4.0f);
        const float rate = 6.0f;
        if (relative) s.attachDampingRate = rate;
        else          s.damping = 1.0f - std::exp(-rate * dt / s.substeps);   // la misma tasa, por subpaso
        const glm::vec3 v(50.0f, 0.0f, 0.0f);
        s.particles.velocity[0] = v;
        glm::vec3 t(0.0f);
        for (int i = 0; i < 180; ++i) {
            t += v * dt;
            s.attachments[0].target = t;
            s.attachments[0].targetVel = v;
            s.step(dt);
        }
        return (double)glm::length(s.particles.position[0] - t);
    };
    const double lagRel = follow(true), lagAbs = follow(false);
    std::printf("    hueso a 50 m/s: rezago %.4f m (relativo) · %.3f m (respecto al mundo)\n", lagRel, lagAbs);
    CHECK(lagRel < 0.05, "amortiguando respecto al hueso, la piel va con el a 50 m/s");
    CHECK(lagAbs > 0.3, "CONTRAPRUEBA: amortiguando respecto al mundo se queda atras (tasa·v/ω² = 0,475 m)");

    // (3) SU PESO LLEGA AL HUESO: quieta y colgando, la reacción media del paso es m·g hacia abajo.
    {
        XPBDSolver s; oneAttached(s, 4.0f);
        s.gravity = glm::vec3(0.0f, -9.81f, 0.0f);
        s.attachDampingRate = 6.0f;
        for (int i = 0; i < 240; ++i) s.step(dt);
        const glm::vec3 f = s.attachments[0].force;
        std::printf("    colgando: reaccion sobre el hueso (%.2f, %.2f, %.2f) N · peso 98,10 N\n", f.x, f.y, f.z);
        CHECK(std::abs(f.y + 98.1f) < 2.0f && std::abs(f.x) < 0.5f, "la reaccion es el peso (m·g hacia abajo)");
        // Contraprueba: con el doble de masa, el doble de fuerza (no es una constante que cuadre por azar).
        XPBDSolver s2; oneAttached(s2, 4.0f);
        s2.particles.inverseMass[0] = 1.0f / 20.0f;
        s2.gravity = glm::vec3(0.0f, -9.81f, 0.0f);
        s2.attachDampingRate = 6.0f;
        for (int i = 0; i < 240; ++i) s2.step(dt);
        std::printf("    con 20 kg: %.2f N\n", s2.attachments[0].force.y);
        CHECK(std::abs(s2.attachments[0].force.y + 196.2f) < 4.0f, "CONTRAPRUEBA: doble masa, doble reaccion");
    }

    // (4) UNA TELA SÓLO TIRA: comprimida se comba (no empuja), estirada vuelve a su largo. Es lo que
    //     deja plegar un ala de membrana sin que la tela empuje a las falanges.
    {
        double longest = 0.0;   // lo más larga que llega a estar tras el primer paso
        auto run = [&](bool stretchOnly, float start) {
            XPBDSolver s;
            s.gravity = glm::vec3(0.0f); s.damping = 0.0f;
            s.particles.add(glm::vec3(0.0f), 0.0f);             // clavada
            s.particles.add(glm::vec3(start, 0.0f, 0.0f), 1.0f);
            Haruka::Physics::XPBD::DistanceConstraint c;
            c.a = 0; c.b = 1; c.restLength = 1.0f; c.compliance = 0.0f; c.stretchOnly = stretchOnly;
            s.distanceConstraints.push_back(c);
            longest = 0.0;
            for (int i = 0; i < 30; ++i) { s.step(dt); longest = std::max(longest, (double)glm::length(s.particles.position[1])); }
            return (double)s.particles.position[1].x;
        };
        const double compOnly = run(true, 0.5f), compBoth = run(false, 0.5f);
        const double strOnly = run(true, 1.5f); const double strLongest = longest;
        std::printf("    comprimida a 0,5 m: se queda en %.3f (solo tira) · %.3f (bilateral) · soltada estirada a 1,5: "
                    "nunca pasa de %.4f m (acaba en %.3f: sin amortiguar, rebota como una cuerda)\n",
                    compOnly, compBoth, strLongest, strOnly);
        CHECK(std::abs(compOnly - 0.5) < 1e-3, "comprimida no empuja: se queda donde estaba");
        CHECK(std::abs(compBoth - 1.0) < 1e-2, "CONTRAPRUEBA: la bilateral la empuja a su largo");
        CHECK(strLongest <= 1.0 + 1e-3, "estirada, tira: tras el primer paso nunca pasa de su largo");
    }

    // (5) TOPE DE ESTIRAMIENTO: una arista blanda (1 Hz) de la que tira una fuerza grande se estiraría
    //     sin fin; con maxStretch 0,1 nunca pasa de 1,1 m, y por debajo del tope sigue siendo blanda.
    {
        auto pull = [&](float maxStretch, float force) {
            XPBDSolver s;
            s.gravity = glm::vec3(0.0f); s.damping = 0.0f;
            s.particles.add(glm::vec3(0.0f), 0.0f);
            s.particles.add(glm::vec3(1.0f, 0.0f, 0.0f), 1.0f);
            Haruka::Physics::XPBD::DistanceConstraint c;
            const float w = 2.0f * 3.14159265f * 1.0f;
            c.a = 0; c.b = 1; c.restLength = 1.0f; c.compliance = 1.0f / (1.0f * w * w); c.maxStretch = maxStretch;
            s.distanceConstraints.push_back(c);
            s.externalForce.assign(2, glm::vec3(0.0f));
            s.externalForce[1] = glm::vec3(force, 0.0f, 0.0f);
            double longest = 0.0;
            for (int i = 0; i < 240; ++i) { s.step(dt); longest = std::max(longest, (double)s.particles.position[1].x); }
            return longest;
        };
        const double capped = pull(0.1f, 200.0f), free = pull(0.0f, 200.0f), gentle = pull(0.1f, 1.0f);
        std::printf("    arista blanda con 200 N: %.3f m con tope 10 %% · %.3f m sin tope · con 1 N y tope: %.4f m\n",
                    capped, free, gentle);
        CHECK(capped <= 1.1 + 1e-3, "con tope, nunca pasa de su largo +10 %");
        CHECK(free > 1.5, "CONTRAPRUEBA: sin tope, la misma fuerza la estira mucho más");
        CHECK(gentle > 1.0 + 1e-4 && gentle < 1.1, "por debajo del tope sigue siendo ELÁSTICA (cede algo, no es rígida)");
    }
}
#else
void test_xpbd_attachment() { beginTest("xpbd_attachment"); std::printf("    (sin modulo SOFTBODY)\n"); }
#endif
