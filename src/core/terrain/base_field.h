#pragma once
/**
 * @file base_field.h
 * @brief GEMELO CPU de `assets/shaders/lib/base_field.glsl`. Cualquier cambio va en los dos.
 *
 * ── POR QUÉ ESTO TIENE QUE EXISTIR ──────────────────────────────────────────────────────────────
 *
 * El bake del planeta (elevación, temperatura, humedad) vive en una retícula de 6 caras de cubo. Lo
 * lee la GPU en tres sitios (`clipmap.tese`, `terrain_node.comp`, `terrain_node.vert`) y hasta ahora
 * la CPU **no lo leía en ninguno**: `TerrestrialPlanet::m_baseHeights` se rellenaba con el comentario
 * "es el suelo que consultará la física" y no lo consultaba nadie.
 *
 * ⚠️ **LA FÍSICA MUESTREA OTRO BAKE.** `sampleHeight` va contra `m_heightCPU`, que es EQUIRECT, no la
 * retícula del cubo. O sea que el suelo que se pisa y el que se dibuja salen de dos mapas distintos,
 * con dos parametrizaciones distintas. Es el hueco que F4 viene a cerrar, y la razón de esta función.
 *
 * ── LA BILINEAL VA A MANO, IGUAL QUE EN EL SHADER ───────────────────────────────────────────────
 *
 * No se usa filtrado de hardware al otro lado, así que aquí tampoco: el bilineal de GL usa pesos de
 * precisión limitada (8 bits en varias GPU) y eso basta para separar los dos suelos en centímetros.
 * Esta es la cuenta EXACTA que hace `harukaSampleBaseField`, en el mismo orden.
 */
#include <algorithm>
#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>

#include "cube_sphere.h"          // dirToCubeFaceClosed — el mismo reparto de caras que el shader
#include "tools/planetary_types.h"

namespace Haruka { namespace Terrain {

/**
 * @brief Altura base (m) en `dir`, de la retícula de 6 caras.
 *
 * @param heights `[face][j][i]` con lado `res+1`, en metros — el `m_baseHeights` del planeta.
 * @param res     celdas por lado (la retícula tiene `res+1` téxeles).
 * @return 0 si no hay retícula, que es el nivel del mar.
 */
inline float baseFieldHeightAt(const float* heights, int res, const glm::dvec3& dir) {
    if (!heights || res <= 0) return 0.0f;
    PlanetFace f; double lx, ly;
    dirToCubeFaceClosed(dir, f, lx, ly);

    const int N   = res;
    const int N1  = res + 1;
    const double fx = (lx * 0.5 + 0.5) * (double)N;
    const double fy = (ly * 0.5 + 0.5) * (double)N;
    int i0 = (int)std::floor(fx), j0 = (int)std::floor(fy);
    i0 = std::min(std::max(i0, 0), N - 1);
    j0 = std::min(std::max(j0, 0), N - 1);
    const double tx = fx - i0, ty = fy - j0;

    const size_t base = (size_t)(int)f * (size_t)N1 * (size_t)N1;
    const float h00 = heights[base + (size_t)j0       * N1 + i0];
    const float h10 = heights[base + (size_t)j0       * N1 + i0 + 1];
    const float h01 = heights[base + (size_t)(j0 + 1) * N1 + i0];
    const float h11 = heights[base + (size_t)(j0 + 1) * N1 + i0 + 1];
    const double a = h00 + (h10 - h00) * tx;
    const double b = h01 + (h11 - h01) * tx;
    return (float)(a + (b - a) * ty);
}

}} // namespace Haruka::Terrain
