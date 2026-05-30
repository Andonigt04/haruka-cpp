#include "terrain_streaming_system.h"

#include "core/lod_system.h" // Para LODUpdate
#include "renderer/terrain_renderer.h" // Para agregar/quitar de la escena

namespace Haruka {
    void TerrainStreamingSystem::reapFinishedTasks() {
        for (auto it = m_asyncTasks.begin(); it != m_asyncTasks.end();) {
            if (it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                it->get();
                it = m_asyncTasks.erase(it);
            } else {
                ++it;
            }
        }
    }

    void TerrainStreamingSystem::update(const std::vector<PlanetChunkKey>& requestedChunks, const nlohmann::json& planetSettings) {
        reapFinishedTasks();

        const std::string planetName = planetSettings.value("planetName", std::string{});
        for (const auto& key : requestedChunks) {
            uint64_t hash = ChunkCache::keyToHash(key);

            if (m_cache.hasChunk(key)) continue;

            {
                std::lock_guard<std::mutex> lock(m_pendingMutex);
                if (m_pendingRequests.count(hash)) continue;
            }

            requestAsyncGeneration(key, planetSettings, planetName);
        }
    }

    void TerrainStreamingSystem::requestAsyncGeneration(const PlanetChunkKey& key, const nlohmann::json& settings, const std::string& planetName) {
        uint64_t hash = ChunkCache::keyToHash(key);
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            m_pendingRequests.insert(hash);
        }

        m_asyncTasks.push_back(std::async(std::launch::async, [this, key, settings, hash, planetName]() {
            auto data = m_generator.generateChunk(key, settings, settings["radius"]);
            data->key        = key;
            data->planetName = planetName;

            // addChunk has its own lock; don't hold resultMutex while it runs.
            m_cache.addChunk(key, *data);

            {
                std::lock_guard<std::mutex> lock(m_resultMutex);
                m_completedChunks.push_back(data);
            }
            {
                std::lock_guard<std::mutex> lock(m_pendingMutex);
                m_pendingRequests.erase(hash);
            }
        }));
    }

    void TerrainStreamingSystem::processLODUpdate(const LODUpdate& update) {
        // 1. ELIMINAR de la GPU lo que ya no es visible
        for (const auto& key : update.chunksToUnload) {
            m_renderer.removeFromScene(update.planetName, key);
        }

        // 2. REFRESCAR en la Caché lo que se mantiene (para que el LRU no lo borre)
        for (const auto& key : update.chunksToKeep) {
            m_cache.getChunk(key); 
        }

        // 3. CARGAR lo nuevo a la GPU
        for (const auto& key : update.chunksToLoad) {
            // Intentamos sacar el dato de la Caché (RAM)
            const auto* data = m_cache.getChunk(key);
            
            if (data) {
                // ¡HIT! El dato ya existe en RAM, lo mandamos directo a la GPU
                m_renderer.addToScene(update.planetName, key, *data);
            }
        }
    }

    std::vector<std::shared_ptr<ChunkData>> TerrainStreamingSystem::getReadyChunks() {
        reapFinishedTasks();

        std::lock_guard<std::mutex> lock(m_resultMutex);
        std::vector<std::shared_ptr<ChunkData>> ready;
        ready.swap(m_completedChunks);
        return ready;
    }
}