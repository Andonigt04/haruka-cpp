/**
 * @file sky_ambient.h
 * @brief Gemelo CPU de `assets/shaders/lib/sky_palette.glsl`: la luz AMBIENTE sale de la MISMA
 *        paleta que el cielo que se dibuja.
 *
 * El cielo procedural tiene dos consumidores que deben partir del MISMO cielo:
 *   · `sky.frag`          → lo DIBUJA (y encima le pone resplandor, halo, nubes y estrellas).
 *   · este módulo (CPU)   → lo INTEGRA en armónicos esféricos para la luz ambiente difusa.
 * Si no comparten paleta, las sombras dejan de corresponderse con lo que se ve (el bug de las
 * "tres constantes" del MEMO 2026-08-05). Ver `docs/guides/PLAN_AMBIENTE_SH.md`.
 *
 * ⚠️ CUALQUIER CAMBIO EN LA PALETA O EN EL GRADIENTE VA EN LOS DOS FICHEROS A LA VEZ.
 */
#pragma once

#include <glm/glm.hpp>

namespace Haruka {

/** Los cuatro colores que definen el cielo. Copia EXACTA de `sky_palette.glsl` (mismas cifras). */
inline constexpr glm::vec3 SKY_ZENITH_NIGHT = glm::vec3(0.012f, 0.016f, 0.045f);
inline constexpr glm::vec3 SKY_HORIZ_NIGHT  = glm::vec3(0.020f, 0.024f, 0.055f);
inline constexpr glm::vec3 SKY_ZENITH_DAY   = glm::vec3(0.18f,  0.40f,  0.82f);
inline constexpr glm::vec3 SKY_HORIZ_DAY    = glm::vec3(0.62f,  0.74f,  0.92f);

/** @brief Factor día/noche. 0 = noche cerrada · 1 = día pleno. Gemelo de `harukaSkyDay` del GLSL.
 *  @param sunElev dot(sunDir, up): elevación del sol sobre el horizonte local. */
float harukaSkyDay(float sunElev);

/** @brief Color base del cielo en una dirección: gradiente horizonte→cénit, interpolado
 *         noche↔día. SIN sol, SIN nubes, SIN estrellas — solo la cúpula. Gemelo de
 *         `harukaSkyBase` del GLSL.
 *  @param t    dot(dir, up): 1 = cénit · 0 = horizonte · <0 bajo el horizonte.
 *  @param day  salida de harukaSkyDay().
 *
 *  Bajo el horizonte se aplana al color de horizonte (para DIBUJAR está bien, lo tapa el
 *  terreno). Para la irradiancia difusa NO sirve ese aplanado: una cara que mira abajo debe
 *  recoger el color del suelo, no azul de horizonte → la integración de `skyAmbientSH` lo
 *  sustituye por `groundColor`. */
glm::vec3 harukaSkyBase(float t, float day);

/**
 * @brief Coeficientes de armónicos esféricos de la IRRADIANCIA difusa del cielo.
 *
 * Índice = l·(l+1)+m, base SH real ortonormal (mismo convenio que la literatura):
 *   [0]=Y00 · [1]=Y1,-1 · [2]=Y10 · [3]=Y11 · [4]=Y2,-2 · [5]=Y2,-1 · [6]=Y20 · [7]=Y21 · [8]=Y22
 *
 * Los coeficientes ya vienen CONVOLUCIONADOS con el kernel difuso de Ramamoorthi & Hanrahan
 * (A0=π, A1=2π/3, A2=π/4, compensados por 4π porque aquí los coeficientes se integran en la
 * convención "media sobre la esfera" — ver el .cpp). Así el evaluador del shader es UN producto
 * escalar: `irradiance(n) = Σ_i coef[i] · Y_i(n)` y devuelve la irradiancia difusa estándar.
 *
 * Como `harukaSkyBase` solo depende de la elevación (simetría azimutal alrededor de `up`), los
 * términos m≠0 salen ~0 numéricamente (verificable por test). Se mantienen los 9 slots para que
 * el plan no pinte al shader de otra forma y para futuros términos direccionales (halo del sol
 * bajo el horizonte, nubes por acimut).
 */
struct SkySH {
    glm::vec3 coef[9];
};

/** @brief Integra `harukaSkyBase` (y el rebote de suelo) a SH de irradiancia difusa.
 *  @param sunElev    elevación solar sobre el horizonte local (alimenta el factor día/noche).
 *  @param cloudCover cobertura de nube 0..1: ATENÚA la luz difusa con un escalar. La forma de
 *                    las nubes no importa para la irradiancia, solo cuánta hay (ver el GLSL).
 *  @param groundColor color del suelo usado bajo el horizonte (rebote). Default: verde tierra
 *                    apagado; el consumidor lo alimentará con la paleta del bioma.
 *  @return 9 coeficientes ya convolucionados (ver `SkySH`).
 *
 *  Coste: cuadratura esférica fina, ~una vez por frame (o por cambio de `sunElev`) — no es el
 *  camino caliente del render. El SOL y el halo NO entran: son luz directa (contarlos dos veces
 *  aquí sería un bug de iluminación, lo documenta también `sky_palette.glsl`). */
SkySH skyAmbientSH(float sunElev,
                   float cloudCover = 0.0f,
                   const glm::vec3& groundColor = glm::vec3(0.15f, 0.17f, 0.13f));

/** @brief Evalúa la irradiancia difusa del cielo en una dirección.
 *  @param sh  salida de `skyAmbientSH`.
 *  @param n   normal (mundo, unitaria) en la que se evalúa.
 *  @param up  cénit local del observador (mundo, unitaria).
 *  @return irradiancia difusa (lineal) en `n`.
 *
 *  El SH se integra alrededor de `up`; `n` se proyecta al triedro local. El fragment shader debe
 *  replicar esta fórmula (producto escalar + la misma base) — es la cara GPU de `SkySH`. */
glm::vec3 skyAmbientEval(const SkySH& sh, const glm::vec3& n, const glm::vec3& up);

} // namespace Haruka
