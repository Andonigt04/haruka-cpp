#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <cstdint>
#include "core/terrain/terrain_sample.h"

/**
 * @file floating_islands.h
 * @brief Fase 5 — islas flotantes: módulo 3D COMPLETAMENTE aparte del heightfield.
 *
 * Un heightfield no puede tener geometría con aire debajo (ver terrain_gen_v2_plan).
 * Las islas flotantes son mallas 3D propias, colocadas por la seed sobre tierra,
 * que flotan a cierta altura. No leen ni modifican el terreno: módulo separado.
 *
 * Forma: blob tipo "isla flotante" — copa achatada arriba, fondo cónico/dentado.
 * Generación y colocación 100% deterministas por (seed, posición).
 */
namespace Haruka {

    struct FloatingIslandMesh {
        std::vector<glm::vec3>    vertices; ///< relativos al centro de la isla (m)
        std::vector<glm::vec3>    normals;
        std::vector<unsigned int> indices;
    };

    struct FloatingIsland {
        glm::dvec3 worldCenter{0.0}; ///< posición absoluta en el mundo (m)
        float      radius = 0.0f;    ///< radio acotador (m) para cull
        FloatingIslandMesh mesh;
    };

    /**
     * @brief Genera la malla de una isla (blob deformado) de tamaño dado.
     * @param radius   radio base (m)
     * @param seed     semilla de la isla (forma determinista)
     * @param sectors  resolución horizontal
     * @param stacks   resolución vertical
     */
    FloatingIslandMesh generateIslandMesh(float radius, uint32_t seed,
                                          int sectors = 24, int stacks = 16);

    /**
     * @brief Coloca las islas flotantes de un planeta (deterministas por seed).
     * @param seed         semilla del mundo
     * @param planetCenter centro del planeta (mundo)
     * @param planetRadius radio del planeta (m)
     * @param W            parámetros del mundo (para gatear sobre tierra)
     */
    std::vector<FloatingIsland> generateFloatingIslands(uint32_t seed,
                                                        const glm::dvec3& planetCenter,
                                                        double planetRadius,
                                                        const WorldGenParams& W);

    /**
     * @brief Islas flotantes CERCA de la cámara (streaming-lite, discoverables).
     *        Rejilla cellular densa en un casquete de radio angular @p angRadius
     *        alrededor de @p camDir. Determinista; regenerar al moverse la cámara.
     * @param camDir    dirección unitaria del planeta a la cámara
     * @param angRadius radio angular del casquete (rad), p.ej. 0.03 ≈ 190 km
     */
    std::vector<FloatingIsland> generateFloatingIslandsNear(uint32_t seed,
                                                            const glm::dvec3& planetCenter,
                                                            double planetRadius,
                                                            const WorldGenParams& W,
                                                            const glm::dvec3& camDir,
                                                            float angRadius);

} // namespace Haruka
