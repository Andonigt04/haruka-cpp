/**
 * @file planet_meso.h
 * @brief F3-MESO del PLAN_TERRENO_V3: el detalle EROSIONADO que sí pisas.
 *
 * El campo macro (PlanetFields) es ~40 km/celda: tiene los valles, las cuencas y los cauces, pero a
 * esa resolución **no los pisas** — entre dos celdas solo hay una interpolación. Al caminar, el
 * mundo es una rampa suave.
 *
 * El MESO refina, BAJO DEMANDA, la zona alrededor del jugador en TESELAS de ~5 km con rejilla de
 * 128² (≈ 40 m/téxel en la Tierra), y las erosiona localmente. Cada tesela:
 *
 *   1. **Upsamplea** el macro (elevación + caudal entrante).
 *   2. Añade detalle (fBm) — pero ya condicionado por el campo.
 *   3. **Erosiona** (priority-flood + D8 + stream power + térmica), igual que el macro.
 *   4. Se **cachea** (LRU).
 *
 * ⚠️ EL RIESGO #1 DEL PLAN, Y CÓMO SE EVITA — las COSTURAS:
 *   - **Caudal HEREDADO**: las celdas del borde no arrancan con "su propia lluvia", sino con el
 *     CAUDAL QUE EL MACRO DICE QUE ENTRA por ahí. Sin esto, un río grande que cruza la tesela
 *     nacería de la nada en el borde y moriría en el otro → cauces cortados entre teselas.
 *   - **MARGEN**: se erosiona una rejilla más grande y se tira el borde. La erosión propaga
 *     información (el agua baja); si erosionas justo hasta el límite, el borde queda con condiciones
 *     de contorno falsas. Con margen, esa contaminación se descarta.
 *
 * PARIDAD: el mismo campo lo muestrean el compute de chunks (GPU, vía atlas SSBO) y el sampler de
 * física (CPU). Si divergen, caminas por encima o por dentro del suelo.
 */
#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <cstdint>

namespace Haruka {

    class PlanetFields;

    /** @brief LA instancia de meso del planeta (una sola caché de teselas para CPU **y** GPU).
     *  Si cada lado tuviera la suya, uno podría tener la tesela y el otro no → la malla usaría el
     *  meso y la física el macro = caminas por encima/dentro del suelo. */
    class PlanetMeso;
    /** @param planetRadius Necesario: el TAMAÑO de la tesela se deriva del radio (ver kTileTargetM).
     *  Una tesela es una fracción de la CARA del cubo, así que con un nivel fijo su tamaño en metros
     *  es proporcional al radio: en un planeta de 5 km medía 3.8 m → cada chunk pedía miles de
     *  teselas y la LRU hacía thrashing eterno (el streaming no terminaba nunca). */
    PlanetMeso& planetMeso(uint32_t seed, const PlanetFields& macro, double planetRadius);

    class PlanetMeso {
    public:
        // Lado de tesela OBJETIVO en metros. El nivel se elige para acercarse a esto sea cual sea el
        // radio del planeta (en la Tierra sale nivel 11 = 2048 teselas por cara ≈ 4.9 km, el valor
        // con el que se diseñó y validó el meso).
        static constexpr double kTileTargetM = 5000.0;
        static constexpr int kMinTileLevel = 1, kMaxTileLevel = 14;
        static constexpr int kTileRes   = 128;  // rejilla interior (≈ 40 m/téxel en la Tierra)
        static constexpr int kMargin    = 16;   // margen que se erosiona y se TIRA (evita costuras)
        // Teselas residentes (LRU). ⚠️ SUBIDO de 192: con 192, la caché se llenaba estando QUIETO y
        // al andar las teselas se evictaban BAJO LOS PIES → la malla se coció con tesela, la física
        // vuelve al macro, y las alturas dejan de coincidir (props flotando, jugador desalineado).
        // Antes no se podía subir: la GPU buscaba la tesela barriendo la tabla ENTERA por vértice.
        // Con la tabla HASH (ver gpuHashTable) buscar es O(1) → el cupo ya no lo ata el shader.
        // Coste: 640 × 128² × 4 B = 42 MB de atlas (RAM y VRAM).
        static constexpr int kMaxTiles  = 640;
        /// Tamaño de la tabla hash (potencia de 2, ~2× el cupo → factor de carga 0.5).
        static constexpr int kHashSize  = 2048;

        void init(uint32_t seed, const PlanetFields* macro, double planetRadius);
        ~PlanetMeso();

        /** @brief Teselas por lado de cara = 1 << tileLevel(). La GPU necesita el MISMO nivel
         *  (uniform u_mesoLevel) o buscaría la tesela equivocada en el atlas. */
        int tileLevel() const { return m_tileLevel; }

        /** @brief Elevación (km) en `dir`. GENERA la tesela si falta (SÍNCRONO, ~10 ms).
         *  Solo para tests/herramientas: en el juego NADIE debe llamar a esto (bloquearía). */
        float elevKm(const glm::vec3& dir);

        /** @brief ¿Está lista la tesela de `dir`? (sin generarla) */
        bool  hasTile(const glm::vec3& dir) const;

        /**
         * @brief ¿Está lista la tesela? Si no, la ENCOLA al worker y devuelve false. NUNCA bloquea.
         *
         * Es la puerta de entrada del juego. Construir una tesela cuesta ~10 ms; hacerlo en el hilo
         * que genera chunks colgaba el arranque (un chunk de LOD bajo abarca media cara del cubo →
         * pedía MILES de teselas). Ahora: el generador PREGUNTA; si la tesela no está, **no genera
         * ese chunk todavía** y lo reintenta en frames siguientes. Mientras tanto se dibuja el
         * ancestro grueso (que ya nunca se evicta) → nunca hay agujero, solo detalle que llega tarde.
         */
        bool requestTile(const glm::vec3& dir);

        /** @brief Teselas pendientes en la cola del worker (telemetría). */
        int  pending() const;

        /** @brief Altura en `dir` SIN generar nada: meso si la tesela está residente, macro si no.
         *  Es lo que usa la FÍSICA — generar bajo demanda aquí sería letal (el spawn consulta 4000
         *  direcciones por todo el planeta). Las teselas las pide el generador de chunks, que es
         *  justo donde hay malla con la que chocar. La GPU hace exactamente lo mismo → paridad. */
        float elevKmIfResident(const glm::vec3& dir) const;

        // --- Puente a GPU: atlas + tabla de teselas residentes ---------------------------------
        // El compute de chunks busca su tesela en la tabla (≤ kMaxTiles) y muestrea el atlas. Si no
        // está, cae al macro — por eso el generador PIDE las teselas antes de despachar.
        struct TileKey { int face, tx, ty; };
        /** @brief Asegura que la tesela de `dir` existe (para que la GPU pueda usarla). */
        void ensureTile(const glm::vec3& dir);
        /** @brief Snapshot COHERENTE para subir a GPU (bajo lock). El worker escribe el atlas en
         *  otro hilo: leerlo sin lock metía basura en las alturas y la malla se iba fuera de sitio
         *  (síntoma: "no se ve el planeta"). Devuelve la tabla y los slots nuevos con sus datos. */
        /** @param outTable TABLA HASH (kHashSize entradas): (face, tx, ty, slot+1); w==0 = vacío.
         *  El compute la sondea con el MISMO hash → O(1) por vértice. */
        void gpuSnapshot(std::vector<glm::ivec4>& outTable,
                         std::vector<int>& outDirtySlots,
                         std::vector<float>& outDirtyData);
        /** @brief Tamaño del atlas en floats (para dimensionar el buffer una vez). */
        static constexpr size_t atlasFloats() { return (size_t)kMaxTiles * kTileRes * kTileRes; }

        bool ready() const { return m_macro != nullptr; }

    private:
        struct Tile { int slot = -1; uint64_t lastUse = 0; };
        uint64_t m_clock = 0;

        uint32_t            m_seed  = 0;
        const PlanetFields* m_macro = nullptr;
        int                 m_tileLevel = 11; // derivado del radio en init()

        std::unordered_map<uint64_t, Tile> m_tiles;   // key empaquetada → slot
        std::vector<float>                 m_atlas;   // kMaxTiles × kTileRes²
        std::vector<glm::ivec4>            m_table;   // kMaxTiles
        std::vector<int>                   m_dirty;

        int  acquireSlot(uint64_t key);               // LRU
        void buildTile(int face, int tx, int ty, std::vector<float>& out); // SIN lock (construye)
        void publishTile(uint64_t key, int face, int tx, int ty, const std::vector<float>& data); // CON lock

        // --- WORKER: construir teselas FUERA del hilo de generación de chunks -------------------
        mutable std::mutex      m_mx;        // protege m_tiles / m_atlas / m_table / m_dirty
        std::deque<uint64_t>    m_queue;     // teselas pedidas, sin construir
        mutable std::mutex      m_qmx;
        std::condition_variable m_cv;
        // VARIOS workers: construir una tesela cuesta ~10 ms y es trabajo de CPU puro e independiente
        // (lee el macro, que es const; solo la PUBLICACIÓN toca estado compartido). Con un único hilo,
        // los chunks se quedaban esperando tesela, se acumulaban en la lista de deseados y el escaneo
        // del streaming crecía con el atasco → picos de frame al encender el meso.
        std::vector<std::thread> m_workers;
        std::atomic<bool>       m_stop{false};
        void workerLoop();
    };

} // namespace Haruka
