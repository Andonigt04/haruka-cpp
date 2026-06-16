#pragma once

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <mutex>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include "core/chunk_cache.h"
#include "tools/math_types.h"

namespace Haruka {

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
            GLuint ebo = 0;
            uint32_t indexCount  = 0;
            uint32_t vertexCount = 0;
            bool isReady = false;
            std::string planetName;
            PlanetChunkKey key{};          // identity, for stale-coverage purge
            glm::dvec3  chunkCenter{0.0};
            float       cullRadius = 0.0f; // bounding-sphere radius for frustum cull
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

    private:
        std::unordered_map<uint64_t, RenderMesh> m_gpuMeshes;
        std::unordered_set<uint64_t>             m_stale;   // mallas a REEMPLAZAR/cubrir
        mutable std::mutex m_renderMutex;
        void purgeStaleCoveredBy(const PlanetChunkKey& key); // caller holds the lock
        glm::mat4  m_cullVP{1.0f};
        bool       m_cullEnabled = false;
        glm::dvec3 m_planetCenter{0.0};
        bool       m_hasPlanetCenter = false;
        double     m_planetRadius = 0.0;
        void cleanupMesh(RenderMesh& mesh);
    };

} // namespace Haruka

