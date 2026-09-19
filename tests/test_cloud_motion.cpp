// ================================================================================================
// EL MOVIMIENTO DE LAS NUBES ENTRE FRAMES: que no vayan a tirones.
//
// Andoni: "va a tirones las nubes". Ningun test miraba dos frames seguidos: todos miran UNA imagen o
// UN estado. Aqui se simulan 3 000 frames (50 s a 60 fps) de lo que llega al shader y se mide el
// SALTO por frame de cada cosa: la deriva del campo (que era `viento x simT`) y el fundido entre
// horneados (que se cambiaba de golpe). Con contraprueba: la formula vieja, con los mismos datos,
// tiene que dar los tirones que se veian, y un salto del reloj no tiene que convertirse en viento.
// ================================================================================================
#include "test_common.h"
#include "core/cloud_motion.h"
#include "core/weather_system.h"

#include <cmath>
#include <cstdio>
#include <vector>

void test_cloud_motion() {
    using namespace Haruka;
    std::printf("\n== nubes: MOVIMIENTO (frame a frame, sin tirones) ==\n");

    // Viento a nivel de nube en unidades del campo por segundo: 4,5 m/s x 2,6 / 1430 m.
    const double kFrame = 1.0 / 60.0;
    const double wUnit  = (double)WeatherSystem::kCloudLevelWindMul * (double)WeatherSystem::kFieldScale;
    uint32_t seed = 77u;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (double)(seed >> 8) / 16777216.0; };

    // ── 1. LA DERIVA: el salto por frame es lo que el viento recorre en un frame, no mas ────────
    {
        CloudDrift drift;
        // Un mundo que lleva 6 horas, con rachas: el viento cambia ±0,3 m/s cada frame (es lo que
        // devuelve `weatherAt` en la camara, que ademas se mueve).
        double simT = 6.0 * 3600.0;
        double maxNew = 0.0, maxOld = 0.0, sumNew = 0.0;
        glm::dvec2 oldPrev(0.0); bool first = true;
        for (int f = 0; f < 3000; ++f) {
            const glm::dvec2 windMs(4.5 + (rnd() - 0.5) * 0.6, 1.0 + (rnd() - 0.5) * 0.6);
            const glm::dvec2 wEN = windMs * wUnit;
            simT += kFrame;
            const glm::dvec2 d = drift.advance(wEN, simT);
            if (f > 0) { maxNew = std::max(maxNew, glm::length(d)); sumNew += glm::length(d); }
            // CONTRAPRUEBA: la formula vieja, viento(ahora) x simT.
            const glm::dvec2 oldNow = wEN * simT;
            if (!first) maxOld = std::max(maxOld, glm::length(oldNow - oldPrev));
            oldPrev = oldNow; first = false;
        }
        const double perFrame = glm::length(glm::dvec2(4.5, 1.0)) * wUnit * kFrame;
        std::printf("    deriva: salto maximo por frame integrada %.5f · media %.5f · lo que recorre el viento en un frame %.5f · formula vieja %.4f (x%.0f)\n",
                    maxNew, sumNew / 2999.0, perFrame, maxOld, maxOld / std::max(maxNew, 1e-12));
        CHECK(maxNew < perFrame * 1.15, "integrada, el campo nunca salta mas de lo que el viento recorre en un frame (+15 % de racha)");
        CHECK(maxOld > maxNew * 50.0, "CONTRAPRUEBA: la formula vieja (viento x simT) daba saltos 50x mayores con los MISMOS datos: eran los tirones");
    }

    // ── 2. UN SALTO DEL RELOJ NO ES VIENTO ───────────────────────────────────────────────────────
    {
        CloudDrift drift;
        const glm::dvec2 wEN = glm::dvec2(4.5, 0.0) * wUnit;
        drift.advance(wEN, 100.0);
        const glm::dvec2 d1 = drift.advance(wEN, 100.0 + kFrame);
        const glm::dvec2 d2 = drift.advance(wEN, 100.0 + kFrame + 3600.0);   // carga, pausa, reloj del cluster
        CHECK(glm::length(d2) <= glm::length(wEN) * CloudDrift::kMaxStepS + 1e-12, "un salto del reloj de una hora avanza como mucho 5 s de viento");
        CHECK(glm::length(d1) < glm::length(d2), "...y sigue avanzando (no se congela)");
    }

    // ── 3. EL FUNDIDO DEL HORNEADO: sube suave y esta en 1 cuando llega el siguiente ────────────
    {
        BakeBlend blend;
        double worldT = 0.0;
        blend.onBake(worldT);                               // el primero: sin anterior
        CHECK(blend.factor(worldT + 0.5) == 1.0, "sin horneado anterior no hay nada que fundir (peso 1)");
        // Horneados cada ~2 s de mundo con el retraso del hilo (50-220 ms), 25 de ellos, y en medio
        // se mira el peso cada frame.
        double maxJump = 0.0, prevF = 1.0; int notReady = 0, n = 0;
        for (int b = 0; b < 25; ++b) {
            const double interval = 2.0 + 0.05 + 0.17 * rnd();
            const double next = worldT + interval;
            for (double t = worldT; t < next; t += kFrame) {
                const double f = blend.factor(t);
                if (n > 0) maxJump = std::max(maxJump, std::fabs(f - prevF));
                prevF = f; ++n;
            }
            // Justo antes de que llegue el siguiente: ¿ha terminado el fundido?
            if (blend.factor(next - 1e-9) < 1.0) ++notReady;
            blend.onBake(next);
            // El cambio de "anterior" con el fundido terminado no salta: de mix(A,B,1)=B a mix(B,C,0)=B.
            const double fAfter = blend.factor(next);
            maxJump = std::max(maxJump, std::fabs(fAfter - 0.0));
            prevF = fAfter;
            worldT = next;
        }
        std::printf("    fundido: %d horneados · salto maximo del peso por frame %.4f · llegaron con el fundido a medias: %d\n", 25, maxJump, notReady);
        CHECK(maxJump < 2.0 * kFrame / (2.0 * BakeBlend::kBlendFraction) + 1e-9, "el peso del horneado nuevo sube suave: por frame, lo que toca (nunca de golpe)");
        CHECK(notReady == 0, "cuando llega el siguiente horneado el fundido anterior YA esta en 1 (si no, el cambio de 'anterior' es un salto)");
        // CONTRAPRUEBA: sin fundido (peso 1 siempre) cada horneado es un salto entero.
        CHECK(std::fabs(1.0 - 0.0) > 0.5, "CONTRAPRUEBA trivial: cambiar la textura de golpe es un salto de peso 1");
    }
}
