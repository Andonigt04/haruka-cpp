/**
 * @file cube_sphere.h
 * @brief La proyección cubo↔esfera (Cobb) y su inversa. **GL-free a propósito.**
 *
 * Vivía dentro de `terrain_generator.cpp`, que arrastra `gpu_heightfield.h` (glad) y por tanto no se
 * puede linkar en `haruka_simbase` — la librería sin GL que usan el módulo de reglas del DGS y el
 * servidor. Sacarla aquí es lo que permite que el SERVIDOR calcule la misma retícula de terreno que
 * el cliente (ver reference_surface.h): si el servidor no puede evaluar la superficie del juego, no
 * puede validar dónde está el suelo.
 *
 * ⚠️ UNA SOLA FÓRMULA. `TerrainGenerator::getLocalPosition` (los vértices de la malla),
 * `faceLocalToDir` y el Newton de `dirToFaceLocal` delegan TODOS aquí. Si dos de ellos divergieran,
 * la física caería en otra celda que el render y volveríamos a "dos terrenos".
 */
#pragma once

#include <glm/glm.hpp>
#include "tools/planetary_types.h"   // PlanetFace (GL-free)

namespace Haruka {

    /** @brief (cara, lx, ly) con lx,ly ∈ [-1,1] → dirección unitaria. Spherify de Cobb: reparte la
     *  distorsión mucho mejor que normalizar el cubo (las esquinas no se estiran). */
    glm::dvec3 cubeFaceToDir(PlanetFace face, double lx, double ly);

    /**
     * @brief INVERSA de cubeFaceToDir, en FORMA CERRADA.
     *
     * Este fichero decía que Cobb "no se invierte en forma cerrada" y usaba semilla gnómica + Newton.
     * Sí se invierte. Con la cara ya elegida, si (a,b) son las dos coordenadas NO dominantes del
     * punto del cubo, el spherify se reduce a
     *     s_j = a·√(½ − b²/6)      s_k = b·√(½ − a²/6)
     * porque la componente dominante vale ±1 y su cuadrado sale de los radicandos. Con A=a², B=b²:
     *     s_j² = A/2 − AB/6        s_k² = B/2 − AB/6
     * y llamando D = 2(s_j²−s_k²) = A−B y T = s_j²+s_k² = S/2 − AB/3 con S = A+B, queda una simple
     * ecuación de segundo grado en S:  S² − 6S + (12T − D²) = 0.
     *
     * La raíz buena es la de abajo (S = A+B ≤ 2 < 3), pero escrita como `3 − √(9−12T+D²)` se CANCELA
     * catastróficamente cerca del centro de la cara, donde T→0 y la raíz→3. De ahí la forma
     * conjugada de abajo. Medido contra ida-y-vuelta de `cubeFaceToDir`: 1e-8 en (lx,ly), que sobre
     * una retícula de 256 son 1e-6 téxeles.
     *
     * Se necesitaba cerrada porque el shader del CLIPMAP tiene que hacer esta misma inversión por
     * vértice teselado, y ahí un Newton con jacobiano numérico son 12 evaluaciones del spherify.
     * El gemelo GLSL es `assets/shaders/lib/cube_face.glsl` — cualquier cambio va en los dos.
     */
    void dirToCubeFaceClosed(const glm::dvec3& dir, PlanetFace& outFace, double& lx, double& ly);

    /** @brief Igual, refinando además con Newton. Es la que usa todo lo que ya existía; hoy la
     *  semilla ya es exacta, así que Newton solo confirma. Se conserva porque es la referencia
     *  contra la que se valida la forma cerrada (test `cube_sphere_inverse`). */
    void dirToCubeFace(const glm::dvec3& dir, PlanetFace& outFace, double& lx, double& ly);

} // namespace Haruka
