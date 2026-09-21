/**
 * @file planet_geology.h
 * @brief F2 del PLAN_TERRENO_V3: TECTÓNICA DE PLACAS — la capa que hace creíble al planeta.
 *
 * El problema que resuelve: hoy la elevación sale de una suma de fBm. Por construcción eso da
 * montañas que son MANCHAS de ruido (no cordilleras), y una costa que es un contorno de ruido.
 * Nada lo explica: no hay causa.
 *
 * Aquí el relieve tiene una CAUSA. Se generan placas sobre la esfera (Voronoi con los bordes
 * deformados por ruido, que si no salen rectos y se ven artificiales), cada una con su corteza
 * (continental u oceánica) y su vector de movimiento. Lo que pasa en el LÍMITE entre dos placas
 * es lo que decide el relieve, y es lo que produce estructuras LINEALES en vez de manchas:
 *
 *   - CONVERGENTE (chocan):
 *       · continental + continental → CORDILLERA (Himalaya).
 *       · continental + oceánica    → la oceánica subduce: FOSA en el mar + ARCO VOLCÁNICO en tierra.
 *       · oceánica   + oceánica     → fosa + ARCO DE ISLAS.
 *   - DIVERGENTE (se separan):
 *       · oceánica → DORSAL oceánica (se levanta el lecho).
 *       · continental → VALLE DE RIFT (se hunde).
 *   - TRANSFORMANTE (deslizan) → fallas, apenas relieve.
 *
 * Y, sobre todo: la tierra deja de ser "ruido por encima de un umbral" y pasa a ser CORTEZA
 * CONTINENTAL. Es la diferencia entre un contorno arbitrario y una costa con sentido.
 *
 * DETERMINISTA por seed: mismas placas en cliente y servidor, sin compartir estado.
 * ANALÍTICO (sin texturas ni caché): ~30 placas → evaluable en cualquier punto, tanto en C++ como
 * en GLSL con la misma tabla de placas. La caché de campos llega en F3, cuando la EROSIÓN la exija
 * (un terreno erosionado ya no es una función cerrada). Ver PLAN_TERRENO_V3 §1.1.
 */
#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

namespace Haruka {

    /** @brief Una placa tectónica. `motion` es tangente a la esfera en `site`. */
    struct TectonicPlate {
        glm::vec3 site{0, 1, 0};    ///< centro de la placa (unitario)
        glm::vec3 motion{0};        ///< velocidad TANGENTE en `site` (unitaria × rapidez)
        float     speed = 0.0f;     ///< rapidez relativa [0,1]
        bool      continental = false;
    };

    /** @brief Lo que la geología dice de una dirección del planeta. */
    struct GeologySample {
        float upliftKm     = 0.0f;  ///< elevación BASE de la corteza (km; <0 = lecho oceánico)
        float orogeny      = 0.0f;  ///< [0,1] cuánto manda el límite aquí (cordillera/fosa/dorsal)
        float boundaryDist = 1.0f;  ///< distancia angular al límite de placa (rad)
        float convergence  = 0.0f;  ///< >0 chocan · <0 se separan · ≈0 deslizan
        bool  continental  = false; ///< corteza continental (→ tierra) u oceánica (→ mar)
        int   plateId      = 0;
    };

    /**
     * @brief Placas del planeta + evaluación del relieve base en cualquier dirección.
     *
     * Coste de `sample`: O(nº de placas) — con ~30 placas es un puñado de productos escalares.
     * Barato de sobra tanto para el generador de chunks como para el sampler de física.
     */
    class PlanetGeology {
    public:
        /** @brief Genera las placas de forma determinista a partir de la seed. */
        void generate(uint32_t seed, int plateCount = 30);

        /** @brief Relieve base de la corteza en `dir` (unitario). */
        GeologySample sample(const glm::vec3& dir) const;

        const std::vector<TectonicPlate>& plates() const { return m_plates; }
        bool empty() const { return m_plates.empty(); }
        uint32_t seed() const { return m_seed; }

        // --- Constantes del modelo (públicas: las usa el port GLSL y los tests) ---------------
        static constexpr float kContinentalBaseKm = 0.35f;   ///< la corteza continental "flota" más alta
        static constexpr float kOceanicBaseKm     = -3.80f;  ///< y la oceánica es más densa → lecho hondo
        static constexpr float kBoundaryWidthRad  = 0.035f;  ///< ancho angular de la zona de límite (~220 km en la Tierra)
        static constexpr float kMaxOrogenyKm      = 6.5f;    ///< altura máxima que aporta una colisión continental
        /// Ancho angular del MARGEN continental (~0.12 rad ≈ 760 km en la Tierra): la corteza se
        /// adelgaza aquí → plataforma + costa irregular. Sin esto, la costa sería el polígono de
        /// la placa (bordes de Voronoi limpios = artificiales).
        static constexpr float kMarginRad         = 0.12f;

    private:
        std::vector<TectonicPlate> m_plates;
        uint32_t m_seed = 0;
    };

} // namespace Haruka
