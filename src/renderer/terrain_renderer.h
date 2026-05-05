#pragma once

#include <unordered_map>
#include <string>
#include <mutex>
#include <glad/glad.h>
#include "game/planetary_system.h"
#include "chunk_cache.h"

namespace Haruka {

    class TerrainRenderer {
    public:
        struct RenderMesh {
            GLuint vao = 0;
            GLuint vbo = 0;
            GLuint ebo = 0;
            uint32_t indexCount = 0;
            bool isReady = false;
        };

        TerrainRenderer() = default;
        ~TerrainRenderer();

        // Llamado por el StreamingSystem cuando un chunk está listo en RAM
        void addToScene(const std::string& planetName, const PlanetChunkKey& key, const PlanetarySystem::ChunkData& data);

        // Llamado por el StreamingSystem cuando el LOD decide ocultar un chunk
        void removeFromScene(const std::string& planetName, const PlanetChunkKey& key);

        // El render principal que corre cada frame
        void render(const glm::mat4& viewProjection, const glm::dvec3& cameraPos);

    private:
        // Usamos el hash de la llave para identificar la malla en la GPU
        std::unordered_map<uint64_t, RenderMesh> m_gpuMeshes;
        std::mutex m_renderMutex;
        
        void cleanupMesh(RenderMesh& mesh);
    };

}