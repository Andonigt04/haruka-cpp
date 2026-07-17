/**
 * @file planet_fields.h
 * @brief F3 del PLAN_TERRENO_V3: EROSIÓN E HIDROLOGÍA — los CAMPOS globales del planeta.
 *
 * Este es el cambio de arquitectura que decidió el usuario: **la altura deja de ser una FÓRMULA y
 * pasa a ser un CAMPO precomputado**. Y no es un capricho: un terreno erosionado ya NO es una
 * función cerrada `elev(dir)` — es el resultado de un proceso iterativo con memoria (el agua
 * arrastra material de aquí y lo deposita allá). No se puede "re-evaluar en un punto".
 *
 * Consecuencia buena: se acaba la duplicación. Antes la misma fórmula vivía en el compute de GPU,
 * en el sampler de CPU y (hasta F1) copiada a mano en el shader del agua. Ahora todos MUESTREAN
 * el mismo campo.
 *
 * Qué se calcula, y por qué en este orden (cada paso necesita el anterior):
 *   1. **Elevación base**: tectónica (PlanetGeology) + colinas.
 *   2. **Relleno de depresiones** (priority-flood): las cuencas sin salida se rellenan hasta que
 *      desbordan. De aquí salen los LAGOS — donde el agua REALMENTE no drena, no por un hash.
 *   3. **Rutado de flujo** (D8) + **acumulación**: cada celda manda su agua a la vecina más baja.
 *      La acumulación es el CAUDAL: de aquí salen los RÍOS (hoy no existen).
 *   4. **Erosión hidráulica** (stream power: E = K·A^m·S^n): el caudal talla. Esto es lo que da
 *      valles en V, gargantas y redes de drenaje dendríticas — lo que el ojo reconoce como montaña
 *      de verdad. Ninguna suma de ruido produce esto.
 *   5. **Erosión térmica**: ninguna ladera pasa del ángulo de reposo → taludes y pedreras.
 *
 * ESCALA (§2 del plan): esto es el nivel MACRO (~20 km/téxel en la Tierra). Da cordilleras, cuencas,
 * grandes ríos y la costa. El detalle que ves al caminar es el nivel MESO, que va después y hereda
 * el caudal de aquí (si no, los valles no casarían entre teselas).
 */
#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

namespace Haruka {

    class PlanetGeology;

    /** @brief Lo que el campo dice de una dirección del planeta. */
    struct FieldSample {
        float elevKm    = 0.0f;   ///< elevación erosionada sobre el nivel del mar (km)
        float flow      = 0.0f;   ///< caudal acumulado (nº de celdas aguas arriba, normalizado)
        float waterKm   = 0.0f;   ///< cota del agua si hay lago aquí (km); 0 = no hay lago
        bool  isLake    = false;
        /// Intensidad orogénica CON SIGNO (orogeny × convergencia): >0 = el límite de placa levanta
        /// montaña aquí. Se HORNEA en el campo a propósito: si no, el generador tendría que evaluar
        /// las 30 placas + el warp de ruido POR VÉRTICE — carísimo y para nada, porque la altura ya
        /// sale del campo. (Era el grueso del coste de reconstruir terreno.)
        float orogeny   = 0.0f;

        // --- CLIMA DERIVADO (F5). No es un ruido: sale de la geografía que ya existe. -----------
        /// Temperatura media (°C): latitud + gradiente vertical (−6.5 °C/km). Manda la NIEVE.
        float tempC     = 15.0f;
        /// Humedad [0,1]: la trae el viento DESDE EL MAR y la descargan las montañas al subir
        /// (sombra orográfica) → detrás de una cordillera hay DESIERTO. Manda el bioma.
        float humidity  = 0.5f;
    };

    /**
     * @brief Campos globales del planeta (cube-sphere, 6 caras × N×N), erosionados y cacheados.
     *
     * `generate()` es CARO (segundos): se hace UNA vez por seed y se cachea. `sample()` es un
     * bilineal: barato, y lo pueden llamar por vértice el generador de chunks y la física.
     */
    class PlanetFields {
    public:
        /** @brief Construye y erosiona los campos. `faceRes` = lado de cada cara del cubo. */
        void generate(uint32_t seed, const PlanetGeology& geo, int faceRes = 512, int erosionIters = 60);

        /** @brief Muestreo bilineal en una dirección (unitaria). */
        FieldSample sample(const glm::vec3& dir) const;

        bool     empty() const { return m_elev.empty(); }
        uint32_t seed()  const { return m_seed; }
        int      faceRes() const { return m_res; }

        /// Umbral de caudal (normalizado) a partir del cual una celda ES un río. Lo usan el render
        /// (cauce/vegetación de ribera) y la colocación de props.
        /// (Con flow = log(acc)/log(accMax): 0 = la celda no recoge nada · 1 = el cauce principal.
        ///  0.45 ≈ cientos de celdas aguas arriba → un río de verdad, no un reguero.)
        static constexpr float kRiverFlow = 0.45f;

    private:
        int      m_res  = 0;
        uint32_t m_seed = 0;
        std::vector<float> m_elev;    ///< 6·res·res — elevación EROSIONADA (km)
        std::vector<float> m_flow;    ///< 6·res·res — caudal normalizado [0,1]
        std::vector<float> m_water;   ///< 6·res·res — cota del lago (km); 0 = sin lago
        std::vector<float> m_oro;     ///< 6·res·res — orogenia con signo (horneada de la tectónica)
        std::vector<float> m_temp;    ///< 6·res·res — temperatura (°C)
        std::vector<float> m_humid;   ///< 6·res·res — humedad [0,1]

        /// Calcula temperatura y humedad SOBRE el terreno ya erosionado (por eso va al final: la
        /// sombra orográfica necesita las cordilleras REALES, no las de antes de erosionar).
        void computeClimate();
    };

} // namespace Haruka
