#pragma once

namespace Haruka { namespace Planet {

/**
 * @brief Resolución de la retícula de una cara del cubo.
 *
 * Aquí vivía además una clase `TerrainGrid`: seis rejillas por cara, pobladas con `heightFn` y
 * pasadas por un erosionador (stream-power + térmica). Nunca se llegó a instanciar — el relieve
 * lo pone hoy `geology.elevationModifier` más el mapa de elevación del autor, y el detalle fino
 * `terrain_detail.h`. Se borró junto con `core/planet/cube_sphere.h`, que era su única razón de
 * existir y que definía una proyección cubo→esfera DISTINTA de la que usa la malla del planeta
 * (`core/terrain/cube_sphere.h`). Tener dos proyecciones vivas es la receta de "dos terrenos", y
 * ésta no la usaba nadie.
 *
 * ⚠️ Lo que queda es un entero con nombre de otra cosa: el único campo que se lee es `faceRes`, y
 * el único sitio que lo rellena lo pone a 256 fijo. Debería ser un campo de la config del planeta,
 * no un struct propio viajando por dos firmas.
 */
struct TerrainGridConfig {
    int faceRes = 256;          // celdas por arista de cara del cubo (total: 6 · res²)
};

}} // namespace Haruka::Planet
