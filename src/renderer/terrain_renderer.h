#pragma once

#include <unordered_map>
#include <string>
#include <mutex>
#include <glad/glad.h>
#include "core/chunk_cache.h"
#include "tools/math_types.h"

namespace Haruka {

    class TerrainRenderer {
    public:
        struct RenderMesh {
            GLuint vao = 0;
            GLuint vbo = 0;
            GLuint nbo = 0;
            GLuint ebo = 0;
            uint32_t indexCount  = 0;
            uint32_t vertexCount = 0;
            bool isReady = false;
            std::string planetName;
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
        void renderPlanet(const std::string& planetName);

        int getGPUMeshCount() const { return static_cast<int>(m_gpuMeshes.size()); }

        struct DrawStats { int draws = 0; int vertices = 0; int triangles = 0; };
        DrawStats getDrawStats() const;
        DrawStats getDrawStatsForPlanet(const std::string& planet) const;

    private:
        // Usamos el hash de la llave para identificar la malla en la GPU
        std::unordered_map<uint64_t, RenderMesh> m_gpuMeshes;
        mutable std::mutex m_renderMutex;
        
        void cleanupMesh(RenderMesh& mesh);
    };

}