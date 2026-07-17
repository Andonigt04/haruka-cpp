#pragma once

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include "rhi/rhi_types.h"
#include <glm/glm.hpp>
#include "core/chunk_cache.h"
#include "tools/math_types.h"

namespace Haruka::RHI { class Device; class Context; }

namespace Haruka {

    class TerrainRenderer; // el agua se sincroniza con el set dibujado del terreno

    /**
     * @brief Renders the planetary ocean shell.
     *
     * Mirrors TerrainRenderer: one mesh per ocean-touching chunk (sea-level grid).
     * Geometry is flat at sea level; Gerstner waves are applied in the vertex
     * shader (Fase 2). Land-only chunks contribute nothing (culled at generation).
     */
    class WaterRenderer {
    public:
        struct RenderMesh {
            // Un buffer por STREAM (= un binding del PSO): posición · normal radial · param
            // (x = nivel km, 0=océano; y = profundidad m) · morph target CDLOD. + índices.
            Haruka::RHI::BufferHandle hVbo, hNbo, hPbo, hMbo, hEbo;
            uint32_t indexCount  = 0;
            uint32_t vertexCount = 0;
            bool isReady = false;
            int  framesUndrawn = 0;        // purga GPU al pasar el margen sin dibujarse
            std::string planetName;
            PlanetChunkKey key{};          // identity, for stale-coverage purge
            glm::dvec3  chunkCenter{0.0};
            float       cullRadius = 0.0f; // bounding-sphere radius for frustum cull
            double      planetRadius = 1.0; // para el factor de morph CDLOD (igual que el terreno)
            // BATCHING (pool): si poolSlot>=0 la geometría vive en un pool compartido (un VAO por
            // vertexCount) y se dibuja con glMultiDrawElementsIndirect → sin rebind de VAO por chunk.
            int32_t     poolSlot = -1;
            uint32_t    poolKey  = 0;      // = vertexCount
        };

        WaterRenderer() = default;
        ~WaterRenderer();

        void addToScene(const std::string& planetName, const PlanetChunkKey& key, const ChunkData& data);
        void removeFromScene(const std::string& planetName, const PlanetChunkKey& key);

        /** @brief Like TerrainRenderer::markStale — keep drawing the old water mesh
         *  until its replacement is ready/covered, so the ocean never flickers a hole
         *  on reload. No-op if the chunk isn't resident. */
        void markStale(const PlanetChunkKey& key);

        /** @brief True si la malla de agua del chunk ya está subida en GPU. */
        bool isResident(const PlanetChunkKey& key) const;
        /** @brief Copia (bajo UN solo lock) los hashes de las mallas de agua residentes+listas.
         *  Para consultar residencia de miles de chunks SIN re-bloquear el mutex por chunk (el
         *  catch-up del agua lo hacía por-chunk → miles de locks/frame = el coste de lod.catchup). */
        void residentHashes(std::unordered_set<uint64_t>& out) const;
        /** @brief Copia (bajo UN lock) los hashes de chunks SIN agua (tierra) que addToScene rechazó.
         *  El catch-up del agua los salta → no re-copia/reintenta chunks de tierra cada pasada. */
        void noWaterHashes(std::unordered_set<uint64_t>& out) const;

        /** Renders this planet's ocean. Ata su propio PSO (el llamador ya subió el UBO per-frame). */
        void renderPlanet(const std::string& planetName, const Haruka::WorldPos& cameraPos);

        /**
         * @brief Parámetros del PASE de agua: oleaje (base tangente + viento + marea) y costa
         * per-píxel (params del generador del planeta activo → paridad exacta con el terreno).
         *
         * Eran ~15 uniforms sueltos que el llamador fijaba con glUniform antes de dibujar. Ahora
         * viajan en el UBO WaterParams (binding 8). Fijar por planeta, antes de renderPlanet.
         * (`oceanSurface` NO está aquí: lo pone el propio renderer según el camino que dibuje.)
         */
        struct PassParams {
            glm::vec3 waveUp{0,1,0}, waveTangent{1,0,0}, waveBitangent{0,0,1};
            glm::vec2 windDir{1.0f, 0.0f};
            float time = 0.0f, windStrength = 1.0f, tideHeight = 0.0f;
            float waterQuality = 1.0f;
            // (F1) Profundidad de la ESCENA + plano cercano: con reversed-Z, dist = near / z → el
            // fragment saca del depth buffer cuánta agua atraviesa el rayo. Sustituye a los params
            // de costa (seed, seaThreshold…) con los que ANTES re-derivaba el terreno per-píxel.
            Haruka::RHI::TextureHandle sceneDepth;
            float     nearPlane = 0.1f;
            glm::vec3 planetRelCam{0.0f};   // ancla de mundo (espuma per-píxel + consulta del campo)
            // (F3) CAMPO del planeta: el agua pregunta "¿hay agua aquí?" en vez de existir en todas
            // partes. `fieldSSBO` es el id GL nativo (lo comparte el generador de terreno).
            unsigned int fieldSSBO = 0;
            int          fieldRes  = 0;
        };
        void setPassParams(const PassParams& p);

        int getGPUMeshCount() const { return static_cast<int>(m_gpuMeshes.size()); }

        /** @brief Sets camera-relative VP for frustum culling (see TerrainRenderer). */
        void setCullMatrix(const glm::mat4& camRelViewProj) { m_cullVP = camRelViewProj; m_cullEnabled = true; }
        /** @brief Planet center (world) + radius for horizon culling. */
        void setPlanetCenter(const glm::dvec3& c) { m_planetCenter = c; m_hasPlanetCenter = true; }
        void setPlanetRadius(double r) { m_planetRadius = r; }
        /** @brief splitFactor REAL del LODSystem (igual que el terreno) para que la banda de
         *  morph CDLOD del agua termine en la frontera de fusión correcta. */
        void setSplitFactor(double sf) { if (sf > 1e-3) m_splitFactor = sf; }

        /** @brief Hojas deseadas del LOD (igual que TerrainRenderer): el agua dibuja, por hoja,
         *  el chunk de agua residente más fino (hoja o ancestro). Antes dibujaba TODOS los
         *  niveles residentes → solape/láminas de agua. Los chunks secos (sin malla de agua)
         *  simplemente no encuentran agua al subir → no se dibuja agua ahí (correcto). */
        void setDesiredLeaves(const std::string& planet, std::vector<PlanetChunkKey> leaves) {
            std::lock_guard<std::mutex> lock(m_renderMutex);
            m_desiredLeaves[planet] = std::move(leaves);
        }

        /** @brief Renderer de terreno con el que SINCRONIZARSE: el agua dibuja el agua de los
         *  MISMOS chunks que dibuja el terreno (no busca su propio ancestro de agua, que pintaba
         *  agua gruesa sobre tierra seca = diamantes flotantes). */
        void setTerrainRenderer(const TerrainRenderer* t) { m_terrain = t; }

    private:
        std::unordered_map<uint64_t, RenderMesh> m_gpuMeshes;
        std::unordered_set<uint64_t>             m_stale;   // mallas a REEMPLAZAR/cubrir
        std::unordered_set<uint64_t>             m_noWater; // chunks de TIERRA (sin océano) → el catch-up los salta
        mutable std::mutex m_renderMutex;
        void purgeStaleCoveredBy(const PlanetChunkKey& key); // caller holds the lock
        double     m_splitFactor = 1.0; // sincronizado con el LODSystem
        const TerrainRenderer* m_terrain = nullptr; // fuente del set dibujado para sincronizar
        std::unordered_map<std::string, std::vector<PlanetChunkKey>> m_desiredLeaves; // hojas LOD por planeta (fallback)
        glm::mat4  m_cullVP{1.0f};
        bool       m_cullEnabled = false;
        glm::dvec3 m_planetCenter{0.0};
        bool       m_hasPlanetCenter = false;
        double     m_planetRadius = 0.0;
        void cleanupMesh(RenderMesh& mesh);

        // --- BATCHING: pool de agua por vertexCount (vértices UNIFORMES = grid (res+1)²;
        // índices VARIABLES = celdas mojadas → cada slot reserva el MÁXIMO de índices y el
        // comando indirecto usa el count real). Un VAO + 4 VBOs + 1 EBO por vertexCount.
        struct WaterPool {
            Haruka::RHI::BufferHandle hPos, hNorm, hParam, hMorph, hEbo;  // 4 streams + índices
            Haruka::RHI::BufferHandle hCmd, hSSBO;   // multidraw: comandos + WDrawItem[] (binding 7)
            uint32_t vertexCount   = 0;   // vértices por slot (grid completo)
            uint32_t maxIndexCount = 0;   // índices reservados por slot (peor caso = todo mojado)
            uint32_t capacity      = 0;   // nº de slots
            std::vector<uint32_t> freeSlots;
        };
        std::unordered_map<uint32_t, WaterPool> m_pools; // clave = vertexCount
        // Agua en per-chunk por DEFECTO (no pool): el pool de agua reserva slots de índice al MÁXIMO
        // (~570 KB/slot) y NO encoge → era el mayor consumidor de VRAM sin dar mejora de water.draw.
        // Per-chunk usa memoria EXACTA y la eviction la libera → VRAM acotada. `terrainpool on` lo
        // reactiva si se quiere probar el MultiDraw del agua.
        bool m_batching = false;
        struct GpuDrawItem { float offMorph[4]; };                                   // 16 B (std430)
        struct GpuDrawCmd  { uint32_t count, instanceCount, firstIndex, baseVertex, baseInstance; }; // 20 B
        std::unordered_map<uint32_t, std::vector<GpuDrawCmd>>  m_scratchCmds;
        std::unordered_map<uint32_t, std::vector<GpuDrawItem>> m_scratchItems;
        WaterPool& getOrCreatePool(uint32_t vertexCount, uint32_t maxIndexCount);
        void growPool(WaterPool& p);

        // --- PSO del agua (water.vert/frag) ------------------------------------------------
        // UNO SOLO para los TRES caminos (océano único · chunk suelto · pool): los tres tienen el
        // MISMO layout de 4 bindings (pos vec3 · normal vec3 · param vec2 · morph vec3). Estado:
        // depth LEQUAL SIN escritura (el terreno bajo el agua se sigue viendo), blend alfa, y SIN
        // culling (la superficie debe verse también desde DEBAJO, bajo el agua).
        Haruka::RHI::PipelineHandle m_pso;
        // UBO del pase (binding 8; el 7 es el SSBO por-draw). Flags como float (std140 GL↔Vulkan).
        struct WaterParamsUBO {
            glm::vec4 waveUpTime{0,1,0,0};      // xyz = up · w = tiempo
            glm::vec4 waveTanWind{1,0,0,1};     // xyz = tangente · w = fuerza del viento
            glm::vec4 waveBitTide{0,0,1,0};     // xyz = bitangente · w = marea (m)
            // xyz = cam − centro del planeta: ANCLA al mundo el ruido de la espuma per-píxel (si se
            // evaluara sobre FragPos, relativo a cámara, la espuma "nadaría"). w = plano cercano
            // (reversed-Z: dist = near / z). NO es la fórmula del terreno: solo un origen estable.
            glm::vec4 relCamNear{0,0,0,0.1f};
            glm::vec2 windDir{1.0f, 0.0f};
            float     waterQuality = 1.0f;
            float     fieldRes = 0.0f;   // (F3) lado de cara del campo; el shader lo usa para saber
                                         // DÓNDE HAY AGUA → sin esto el mar existe bajo la tierra
        };
        static_assert(sizeof(WaterParamsUBO) == 80, "std140: 4×vec4 + vec2 + 2 floats");
        WaterParamsUBO             m_params{};
        Haruka::RHI::BufferHandle  m_uboParams;
        Haruka::RHI::TextureHandle m_sceneDepth;   // depth de la escena (binding 6)
        unsigned int               m_fieldSSBO = 0; // campo del planeta (binding 9)
        // SSBO de UN elemento: el océano único y los chunks sueltos se dibujan con un drawIndexed
        // (gl_DrawID = 0) → leen el item 0. Mismo shader que el camino indirecto, sin ramas.
        Haruka::RHI::BufferHandle m_soloSSBO;
        /** @brief Crea (una vez) el PSO + UBO + SSBO de 1 elemento. Devuelve false si no hay device. */
        bool ensurePipeline();
        /** @brief Ata pipeline + UBO del pase + depth de escena. Común a los tres caminos. */
        Haruka::RHI::Context* beginPass();

    public:
        void setBatching(bool b) { std::lock_guard<std::mutex> lk(m_renderMutex); m_batching = b; }
        bool getBatching() const { return m_batching; }

        // --- SUPERFICIE ÚNICA de océano (test, `oceanshell on/off`) ---
        // En vez de mallas de agua por-chunk, dibuja UNA rejilla a nivel del mar que sigue a la
        // cámara (densa cerca, cubre hasta el horizonte). El recorte de costa per-píxel (oceanElevKm)
        // + el depth del terreno la limitan a donde hay mar → sin teselas, detalle uniforme. Cimiento
        // para acoplar la sim de fluidos (F5). Toggle para comparar y decidir si sustituye al por-chunk.
        void setSingleOcean(bool b) { m_singleOcean = b; }
        bool getSingleOcean() const { return m_singleOcean; }
        void renderSingleOcean(const std::string& planet, const Haruka::WorldPos& cameraPos);

        // F5.1 — ACOPLE A LA SIM DE FLUIDOS: por cada vértice del océano único, el juego dice si cae en
        // el parche de la sim y, si sí, cuánto se desplaza el nivel respecto al mar (m) + peso de mezcla
        // (fade en el borde). Así el mar CERCA del jugador lo mueve la sim (olas/costa/cavar), lejos =
        // nivel del mar estático. Callback = puente engine↔juego (el juego posee la sim).
        void setFluidSampler(std::function<bool(const glm::dvec3&, float&, float&)> f) { m_fluidSample = std::move(f); }
    private:
        Haruka::RHI::BufferHandle m_hOceanPos, m_hOceanNorm, m_hOceanParam, m_hOceanMorph, m_hOceanEbo;
        int    m_oceanIdxCount = 0;
        bool   m_singleOcean = true; // DEFAULT: océano = superficie única (aprobado: sin teselas). `oceanshell off` = por-chunk.
        std::function<bool(const glm::dvec3&, float&, float&)> m_fluidSample; // F5.1: acople a la sim
        std::vector<glm::vec3> m_scPos, m_scNorm, m_scMorph;  // scratch del océano único (reuso, sin realocar/frame)
        std::vector<glm::vec2> m_scParam;
        // CACHE del rebuild: la geometría del océano único solo cambia si la cámara se MUEVE o si el
        // parche de la sim (F5.1) tocó algún vértice. Quieto y en mar abierto (sin parche) las olas
        // animan en el shader (u_time) → nos saltamos el fill de 37k vértices + 4 uploads y redibujamos.
        glm::dvec3 m_ocLastCamd{1e30};   // cámara del último rebuild (mundo)
        glm::dvec3 m_ocSubBuilt{0.0};    // sub-punto del mar en el build (para offset exacto entre rebuilds)
        bool       m_ocBuilt = false;    // ya hay geometría subida
        bool       m_ocFluidWasActive = false; // el último build aplicó delta de sim → seguir reconstruyendo
    };

} // namespace Haruka

