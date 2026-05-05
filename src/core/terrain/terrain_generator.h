#pragma once

#include <memory>
#include <glm/glm.hpp>

#include "scene/scene_manager.h" // Para leer terrainSettings
#include "game/planetary_system.h" // Para PlanetChunkKey y ChunkData

namespace Haruka {

    class TerrainGenerator {
    public:
        TerrainGenerator() = default;

        /**
         * @brief Genera la malla de un chunk específico.
         * Esta función debería ser agnóstica al hilo (Thread-safe).
         */
        std::shared_ptr<PlanetarySystem::ChunkData> generateChunk(
            const PlanetChunkKey& key, 
            const nlohmann::json& settings,
            double planetRadius
        );

    private:
        // Métodos internos para calcular ruido (Noise)
        float calculateHeight(const glm::vec3& posOnSphere, const nlohmann::json& layers);
        
        // Convierte coordenadas de Chunk (X,Y) a posición 3D en la cara del cubo
        glm::vec3 getLocalPosition(const PlanetChunkKey& key, int x, int y, int chunkSize);
    };

}