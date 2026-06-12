#pragma once

#include <glm/glm.hpp>
#include "terrain_sample.h"

/**
 * @file terrain_sampler_v2.h
 * @brief Sampler de terreno v2 (Fase 2b): evalúa altura + normal ANALÍTICA en un punto.
 *
 * Estructura A→B (ver terrain_gen_v2_plan):
 *   A (binario gate) → landMask = smoothstep(continentalness)
 *   B (relieve)      → gateado por A: rama mar (curva) y rama tierra (base + montañas
 *                      ridged ponderadas por régimen). mix por landMask en la costa.
 *
 * La normal NO usa diferencias finitas: sale del gradiente analítico de la elevación
 * (autodiff forward por todos los términos) → seam-safe y sin pinchos por diseño.
 */
namespace Haruka {

    /**
     * @brief Evalúa el terreno en una dirección esférica unitaria.
     * @param dir          Dirección unitaria sobre la esfera (se normaliza por dentro).
     * @param W            Parámetros del mundo (derivados de la seed).
     * @param planetRadius Radio del planeta en metros (para la normal y el nivel del mar).
     * @return TerrainSample con elevKm, normal analítica, landMask y datos de agua.
     */
    TerrainSample sampleTerrainV2(const glm::vec3& dir, const WorldGenParams& W, double planetRadius);

    /**
     * @brief Deriva los parámetros del mundo de la seed (una vez, al cargar el planeta).
     *
     * Knobs acotados y coherentes: ratio mar/tierra (calibra seaThreshold por CDF
     * empírica de la continentalidad para clavar oceanFraction) y escala de
     * continentes (un factor gobierna count+freq+densidad Voronoi).
     */
    WorldGenParams deriveWorldParams(uint32_t seed, double planetRadius);

} // namespace Haruka
