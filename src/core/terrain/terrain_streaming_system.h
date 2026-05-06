#pragma once

#include <queue>
#include <future>
#include <mutex>
#include <chrono>
#include <unordered_set>
#include "core/chunk_cache.h"
#include "core/lod_system.h"
#include "core/terrain/terrain_generator.h"
#include "renderer/terrain_renderer.h"
#include "tools/planetary_types.h"

namespace Haruka {

    class TerrainStreamingSystem {
    public:
        TerrainStreamingSystem(ChunkCache& cache, TerrainGenerator& generator, TerrainRenderer& renderer)
            : m_cache(cache), m_generator(generator), m_renderer(renderer) {}

        /**
         * @brief Procesa las peticiones del LOD.
         */
        void update(const std::vector<PlanetChunkKey>& requestedChunks, const nlohmann::json& planetSettings);

        /**
         * @brief Aplica los cambios de LOD a la escena (Agregar/Quitar chunks).
         */
        void processLODUpdate(const LODUpdate& update);

        /**
         * @brief Recupera los chunks que ya terminaron de generarse.
         */
        std::vector<std::shared_ptr<ChunkData>> getReadyChunks();


    private:
        ChunkCache& m_cache;
        TerrainGenerator& m_generator;
        TerrainRenderer& m_renderer;

        // Chunks que están siendo generados actualmente
        std::unordered_set<uint64_t> m_pendingRequests;
        std::mutex m_pendingMutex;
        std::vector<std::future<void>> m_asyncTasks;
        
        // Resultados listos para ser inyectados en la escena
        std::vector<std::shared_ptr<ChunkData>> m_completedChunks;
        std::mutex m_resultMutex;

        void requestAsyncGeneration(const PlanetChunkKey& key, const nlohmann::json& settings);
        void reapFinishedTasks();
    };

}