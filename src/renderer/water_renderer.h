#pragma once

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include "core/chunk_cache.h"
#include "tools/math_types.h"

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
            GLuint vao = 0;
            GLuint vbo = 0;   // positions (relative to chunkCenter)
            GLuint nbo = 0;   // radial normals
            GLuint pbo = 0;   // per-vertex water level (km): 0=océano, >0=lago
            GLuint mbo = 0;   // CDLOD morph target (posición en el LOD padre)
            GLuint ebo = 0;
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

        /** Renders this planet's ocean. Caller binds the water shader + UBO first. */
        void renderPlanet(const std::string& planetName, const Haruka::WorldPos& cameraPos);

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
            GLuint   vao = 0;
            GLuint   posVBO = 0, normVBO = 0, paramVBO = 0, morphVBO = 0, ebo = 0;
            uint32_t vertexCount   = 0;   // vértices por slot (grid completo)
            uint32_t maxIndexCount = 0;   // índices reservados por slot (peor caso = todo mojado)
            uint32_t capacity      = 0;   // nº de slots
            std::vector<uint32_t> freeSlots;
            GLuint   cmdBuf   = 0;         // GL_DRAW_INDIRECT_BUFFER (por frame)
            GLuint   drawSSBO = 0;         // WDrawItem[] (binding 7)
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
        void setupPoolVAO(WaterPool& p);

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
        GLuint m_oceanVao = 0, m_oceanPos = 0, m_oceanNorm = 0, m_oceanParam = 0, m_oceanMorph = 0, m_oceanEbo = 0;
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

