#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <limits>

/**
 * @file terrain_sample.h
 * @brief Contrato modular de la generación de terreno v2 (Fase 0).
 *
 * Define las DOS estructuras que pegan todo el rediseño:
 *   - WorldGenParams: parámetros del mundo derivados UNA vez de la seed.
 *   - TerrainSample : el resultado de evaluar el terreno en un punto.
 *
 * Filosofía (ver memoria terrain_gen_v2_plan):
 *   - UNA seed decide toda la geografía; todo determinista, función pura de (dir, seed).
 *   - Orden canónico A→B: ruido A (binario) = GATE mar/tierra; ruido B = relieve, gateado por A.
 *   - El agua se reconcilia: hay agua solo donde A dice agua Y el suelo está bajo el nivel.
 *   - TODOS los consumidores (normales, lagos, biomas, agua, colisión) leen un
 *     TerrainSample SIN recalcular el ruido. Ese es el punto de la modularidad.
 *
 * Cada campo indica en qué FASE se rellena. Las fases aún no implementadas dejan
 * el valor por defecto documentado abajo.
 */
namespace Haruka {

    /** @brief Tipo de lámina de agua aplicable en un punto. */
    enum class WaterType : uint8_t {
        None  = 0,  ///< sin agua (tierra seca por encima del nivel)
        Ocean = 1,  ///< océano global, salado, nivel 0
        Lake  = 2,  ///< lago/charco dulce, nivel LOCAL (>0)
    };

    /**
     * @brief Parámetros del mundo derivados de la seed (una vez, al cargar el planeta).
     *
     * Knobs acotados y COHERENTES (decisión de diseño): la seed elige, pero dentro
     * de rangos plausibles para no generar mundos degenerados. Un único factor de
     * escala gobierna a la vez nº de continentes, frecuencia del ruido A y densidad
     * Voronoi, para que no se contradigan.
     *
     * La escena puede OVERRIDEAR cualquiera de estos para mundos hechos a mano;
     * lo derivado de la seed es solo el default.
     */
    struct WorldGenParams {
        uint32_t seed = 0;

        // --- Perfil del cuerpo (gobierna TODO el sampler) ---
        // 0 = terran (continentes/montañas/océano/lagos), 1 = moon (cráteres, gris,
        // SIN mar), 2 = gas (bandas suaves, sin relieve duro). Lo fija la escena vía
        // terrainSettings.config.profile ("terran"/"moon"/"gas").
        int profile = 0;

        // --- Etapa 1: geografía (ruido A, binario base) ---
        float oceanFraction = 0.60f;  ///< fracción de superficie que es mar  (hash seed, ~[0.45,0.75])
        float seaThreshold  = 0.0f;   ///< umbral de continentalidad (c0) que produce oceanFraction
        // (Aquí estaban `coastBandKm` y `coastWidth`, que parametrizaban la banda y las rampas de
        //  costa cuando la costa era un smoothstep sobre el ruido. Desde F2 la costa ES elev = 0 y la
        //  banda es fija (±2 m, ver sampleTerrainV2): ninguno de los dos tenía ya un solo lector.)

        // Escala de continentes — UN solo factor s∈[0,1] (hash seed) gobierna los tres:
        int   continentCount = 5;     ///< nº de continentes  (~[3,8])
        float continentFreqA = 1.2f;  ///< frecuencia del ruido A (sube con continentCount)
        float voronoiDensity = 1.0f;  ///< densidad de puntos Voronoi para ~continentCount regiones

        // --- Etapa 2: relieve (ruido B) ---
        float reliefStrength = 1.0f;  ///< intensidad del relieve montañoso (1 = ~7.5 km pico). Parámetro de escena: config.reliefStrength.

        // --- Etapa 2.5: hidrología ---
        // (Aquí estaban `lakeDensity` y `lakeMaxProb`, que sembraban los "sellos de lago" por hash.
        //  Desde F3 los lagos salen del CAMPO —priority-flood sobre la topografía erosionada, o sea,
        //  donde de verdad no drena— y esos dos parámetros dejaron de tener lector. Un lago no está
        //  donde un hash diga.)
    };

    /**
     * @brief Resultado de evaluar el terreno en una dirección esférica.
     *
     * Es la salida del sampler y la ENTRADA de todos los consumidores. Una sola
     * evaluación por punto; nadie recalcula el ruido por su cuenta.
     */
    struct TerrainSample {
        // ============ Etapa 1 — geografía (ruido A, gate) ============
        float landMask    = 1.0f;   ///< [0,1] SUAVE. 0 = mar/agua, 1 = tierra. Gate maestro.
        int   continentId = -1;     ///< id de continente (Voronoi). -1 = océano.
        float distToCoast = 0.0f;   ///< distancia con signo a la costa: <0 tierra adentro · 0 costa · >0 mar (km)
        float wetness     = 0.0f;   ///< [0,1] potencial hidrológico del continente (lagos/charcos)
        float climate     = 0.5f;   ///< [0,1] clima: 0 frío (polos/altura) · 1 cálido. De latitud + ruido.
        // ============ F5 — CLIMA DERIVADO (del campo del planeta) ============
        // NO son ruido: temperatura = latitud + altitud; humedad = advección desde el mar CON sombra
        // orográfica (detrás de una cordillera llega seca). Es lo que decide el BIOMA — y por tanto
        // dónde van los props: bosque donde llueve, no "a tal altitud".
        float tempC       = 15.0f;  ///< temperatura media (°C)
        float humidity    = 0.5f;   ///< [0,1] humedad
        float flow        = 0.0f;   ///< [0,1] caudal normalizado (>0.45 = río; ver PlanetFields::kRiverFlow)
        float orogeny     = 0.0f;   ///< intensidad orogénica (límite de placa CONVERGENTE): >0 = cordillera/falla.
                                    ///< Las VETAS minerales siguen esto — dejan de aparecer "por altitud".

        // ============ Etapa 2 — relieve (ruido B, gateado por A) ============
        float elevKm = 0.0f;        ///< elevación FINAL sobre el nivel del mar (km). Mar<0, tierra>0. Render y colisión usan ESTE valor.

        // ============ Fase 1 — normal analítica ============
        glm::vec3 normal{0, 1, 0};  ///< normal por gradiente analítico, hacia afuera. (Placeholder hasta Fase 1.)

        // ============ Agua (reconciliada) ============
        WaterType waterType   = WaterType::None;                       ///< qué agua aplica aquí
        float     waterLevelKm = -std::numeric_limits<float>::infinity(); ///< nivel de la lámina (km). 0=océano, >0=lago. -inf si ninguna.

        /** @brief ¿Hay agua visible? A dice agua Y el suelo está bajo el nivel. */
        bool isWater() const { return waterType != WaterType::None && elevKm < waterLevelKm; }

        // Etapa 4 (material/bioma de superficie) se derivará de elevKm + normal(pendiente)
        // + climate + wetness en el shader/material; no necesita campos extra aquí de momento.
    };

    // Stub: terrain sampler removed. Returns flat terrain at elevKm=0.
    inline TerrainSample sampleTerrainV2(const glm::vec3&, const WorldGenParams&, double) {
        return TerrainSample{};
    }

    // Stub: deriveWorldParams — returns default WorldGenParams from seed.
    inline WorldGenParams deriveWorldParams(uint32_t seed, double /*radius*/) {
        WorldGenParams W{};
        W.seed = seed;
        return W;
    }

} // namespace Haruka
