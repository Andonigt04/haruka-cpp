#pragma once

#include <unordered_map>
#include <string>
#include <mutex>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include "core/chunk_cache.h"
#include "tools/math_types.h"

namespace Haruka {

    class TerrainRenderer {
    public:
        struct RenderMesh {
            GLuint vao = 0;
            GLuint vbo = 0;
            GLuint mbo = 0;   // morph-target buffer (CDLOD)
            GLuint nbo = 0;
            GLuint uvo = 0;   // UV buffer
            GLuint ebo = 0;
            uint32_t indexCount  = 0;
            uint32_t vertexCount = 0;
            bool isReady = false;
            std::string planetName;
            PlanetChunkKey key{};         // identity, for invalidation on terrain edit
            glm::dvec3  chunkCenter{0.0};
            int         terrainMode = 0;  // 0=Procedural, 1=Manual
            int         lod = 0;          // chunk LOD level, for morph-band calc
            double      planetRadius = 1.0;
            // Real bounding sphere computed from the vertices (relative to
            // chunkCenter). Used for accurate frustum culling — the chunk's actual
            // geometry sits at terrain elevation, NOT at the sea-level chunkCenter,
            // so a sea-level estimate wrongly culls visible chunks.
            glm::vec3   bsCenter{0.0f};   // sphere centre, relative to chunkCenter
            float       bsRadius = 0.0f;  // sphere radius (metres)
        };

        TerrainRenderer() = default;
        ~TerrainRenderer();

        // Llamado por el StreamingSystem cuando un chunk está listo en RAM
        void addToScene(const std::string& planetName, const PlanetChunkKey& key, const ChunkData& data);

        // Llamado por el StreamingSystem cuando el LOD decide ocultar un chunk
        void removeFromScene(const std::string& planetName, const PlanetChunkKey& key);

        // Render completo (sin modelo por planeta)
        void render(const Haruka::WorldPos& cameraPos);

        // Render de un planeta específico (el caller ya subió el UBO model matrix)
        void renderPlanet(const std::string& planetName, const Haruka::WorldPos& cameraPos);

        /**
         * @brief Sets the camera-relative view-projection used for frustum culling.
         * Geometry is drawn camera-relative (projection * mat3(view)), so pass that
         * same matrix here. Chunks whose bounding sphere is fully outside the
         * frustum are skipped — large win when >200 chunks exist but only a
         * fraction are on screen. Call once per frame before renderPlanet.
         */
        void setCullMatrix(const glm::mat4& camRelViewProj) { m_cullVP = camRelViewProj; m_cullEnabled = true; }

        /** @brief Planet center (world, double) for horizon culling. Per planet, before renderPlanet. */
        void setPlanetCenter(const glm::dvec3& c) { m_planetCenter = c; m_hasPlanetCenter = true; }

        /** @brief Removes GPU meshes whose bounding sphere overlaps (center,radius)
         *  and returns their chunk keys, so the caller can drop them from the cache
         *  and let streaming regenerate them (with the new terrain edit applied). */
        std::vector<PlanetChunkKey> invalidateSphere(const glm::dvec3& center, double radius);

        int getGPUMeshCount() const { return static_cast<int>(m_gpuMeshes.size()); }
        int getLastDrawnCount() const { return m_lastDrawn; }

        struct DrawStats { int draws = 0; int vertices = 0; int triangles = 0; };
        DrawStats getDrawStats() const;
        DrawStats getDrawStatsForPlanet(const std::string& planet) const;

    private:
        // Usamos el hash de la llave para identificar la malla en la GPU
        std::unordered_map<uint64_t, RenderMesh> m_gpuMeshes;
        mutable std::mutex m_renderMutex;

        glm::mat4  m_cullVP{1.0f};
        bool       m_cullEnabled = false;
        glm::dvec3 m_planetCenter{0.0};
        bool       m_hasPlanetCenter = false;
        int        m_lastDrawn = 0;

        void cleanupMesh(RenderMesh& mesh);
    };

}