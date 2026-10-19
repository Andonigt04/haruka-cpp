/**
 * @file SkyAmbient.h
 * @brief La luz AMBIENTE del cielo en armónicos esféricos (SH), para quien la evalúa por normal
 *        (`lib/sky_sh.glsl`, el planeta lejano de `biome.frag`).
 *
 * ⚠️ AQUÍ VIVÍA EL GEMELO DE UNA PALETA: cuatro colores de cielo (`sky_palette.glsl`) integrados a
 * SH. La paleta ya no existe — el cielo sale de la atmósfera (`World/Planet/Atmosphere.h`) y su luz
 * difusa la calcula `Renderer/SkyLight` como irradiancia sobre una cara que mira ARRIBA y otra que
 * mira ABAJO. Este módulo solo pasa esas dos cifras a SH y evalúa el SH, que es lo que los shaders
 * leen.
 */
#pragma once

#include <glm/glm.hpp>

namespace Haruka::Core {

/**
 * @brief Coeficientes de armónicos esféricos de la IRRADIANCIA difusa del cielo.
 *
 * Índice = l·(l+1)+m, base SH real ortonormal:
 *   [0]=Y00 · [1]=Y1,-1 · [2]=Y10 · [3]=Y11 · [4]=Y2,-2 · [5]=Y2,-1 · [6]=Y20 · [7]=Y21 · [8]=Y22
 * Ya CONVOLUCIONADOS con el kernel difuso: `irradiance(n) = Σ coef[i] · Y_i(n)`, un producto escalar.
 * En el marco local del observador (z = vertical); ver `skyAmbientEval`.
 */
struct SkySH {
    glm::vec3 coef[9];
};

/**
 * @brief SH de un ambiente de CIELO arriba y SUELO abajo: `E(n) = (arriba+abajo)/2 + (arriba−abajo)/2·z`.
 *
 * Es exactamente lo que da un cielo uniforme sobre un suelo uniforme, y cabe ENTERO en los términos
 * L0 y L1 (los demás son cero).
 * @param up    irradiancia sobre una cara que mira al cénit.
 * @param down  irradiancia sobre una cara que mira al nadir.
 */
SkySH skyHemisphereSH(const glm::vec3& up, const glm::vec3& down);

/** @brief Evalúa la irradiancia difusa en la normal `n` (mundo, unitaria), con vertical `up`.
 *  El fragment shader replica esta fórmula (`harukaSkySHEval`). */
glm::vec3 skyAmbientEval(const SkySH& sh, const glm::vec3& n, const glm::vec3& up);

} // namespace Haruka::Core
