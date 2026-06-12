#pragma once

#include <memory>
#include <glm/glm.hpp>

#include "core/scene/scene_manager.h" // Para leer terrainSettings
#include "tools/planetary_types.h" // Para PlanetChunkKey y ChunkData

namespace Haruka {

    class DeformationField;

    class TerrainGenerator {
    public:
        TerrainGenerator() = default;

        // Scales the noise octave counts used per vertex (lower = cheaper chunk
        // generation, slightly smoother terrain). Driven by terrainQuality.
        // 1.0 = full detail. Applies to chunks generated after the change.
        static inline float s_detailScale = 1.0f;

        /** @brief Player-edit layer (craters/dig/build). Non-owning; may be null.
         *  Read from worker threads during generateChunk → must outlive generation
         *  and only be mutated on the main thread between frames. */
        void setDeformationField(const DeformationField* f) { m_deform = f; }

        /**
         * @brief Genera la malla de un chunk específico.
         * Esta función debería ser agnóstica al hilo (Thread-safe).
         */
        std::shared_ptr<ChunkData> generateChunk(
            const PlanetChunkKey& key,
            const nlohmann::json& settings,
            double planetRadius
        );

        /**
         * @brief Returns the terrain height offset (in metres) above the
         *        reference sphere surface at the given unit-sphere direction.
         * @param sphereDir  Normalised direction on the unit sphere.
         * @param settings   Planet terrain settings JSON (same format used by generateChunk).
         * @param planetRadius Planet radius in metres.
         */
        float sampleHeightAt(const glm::vec3& sphereDir,
                             const nlohmann::json& settings,
                             double planetRadius);

    private:
        // Métodos internos para calcular ruido (Noise)
        float calculateHeight(const glm::vec3& posOnSphere, const nlohmann::json& layers);
        
        // Convierte coordenadas de Chunk (X,Y) a posición 3D en la cara del cubo
        glm::dvec3 getLocalPosition(const PlanetChunkKey& key, int x, int y, int chunkSize);

        const DeformationField* m_deform = nullptr;
    };

}