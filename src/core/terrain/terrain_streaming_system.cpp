#include "terrain_streaming_system.h"

#include "core/lod_system.h" // Para LODUpdate
#include "renderer/terrain_renderer.h" // Para agregar/quitar de la escena

namespace Haruka {
    void TerrainStreamingSystem::reapFinishedTasks() {
        for (auto it = m_asyncTasks.begin(); it != m_asyncTasks.end();) {
            if (it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                try { it->get(); } catch (...) { /* failure already handled in-task */ }
                it = m_asyncTasks.erase(it);
            } else {
                ++it;
            }
        }
    }

    void TerrainStreamingSystem::setDesiredChunks(std::vector<PlanetChunkKey> sortedNearToFar,
                                                  const nlohmann::json& planetSettings) {
        {
            std::lock_guard<std::mutex> lock(m_desiredMutex);
            m_desiredChunks     = std::move(sortedNearToFar);
            m_desiredSettings   = planetSettings;
            m_desiredPlanetName = planetSettings.value("planetName", std::string{});
        }
        pump();
    }

    void TerrainStreamingSystem::pump() {
        reapFinishedTasks();

        std::lock_guard<std::mutex> dlock(m_desiredMutex);
        if (m_desiredChunks.empty()) return;

        for (const auto& key : m_desiredChunks) { // ya viene cercano→lejano
            // Respetar el presupuesto de concurrencia (cercanos primero).
            {
                std::lock_guard<std::mutex> lock(m_pendingMutex);
                if (m_maxInFlight != 0 && m_pendingRequests.size() >= m_maxInFlight) break;
            }
            uint64_t hash = ChunkCache::keyToHash(key);
            if (m_cache.hasChunk(key)) continue; // ya generado en RAM
            {
                std::lock_guard<std::mutex> lock(m_pendingMutex);
                if (m_pendingRequests.count(hash)) continue; // ya en vuelo
                if (m_failedChunks.count(hash))    continue; // generación falló antes → no reintentar en bucle
            }
            requestAsyncGeneration(key, m_desiredSettings, m_desiredPlanetName);
        }
    }

    void TerrainStreamingSystem::requestAsyncGeneration(const PlanetChunkKey& key, const nlohmann::json& settings, const std::string& planetName) {
        uint64_t hash = ChunkCache::keyToHash(key);
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            m_pendingRequests.insert(hash);
        }

        m_asyncTasks.push_back(std::async(std::launch::async, [this, key, settings, hash, planetName]() {
            bool ok = false;
            try {
                auto data = m_generator.generateChunk(key, settings, settings["radius"]);
                data->key        = key;
                data->planetName = planetName;

                // addChunk has its own lock; don't hold resultMutex while it runs.
                m_cache.addChunk(key, *data);

                {
                    std::lock_guard<std::mutex> lock(m_resultMutex);
                    m_completedChunks.push_back(data);
                }
                ok = true;
            } catch (...) {
                // A throwing chunk must NOT leak its in-flight slot — that would
                // eventually fill the budget and stall ALL streaming (visible as
                // permanent holes / GPU count frozen). Mark it failed so we don't
                // spin retrying it, and always release the slot below.
            }
            {
                std::lock_guard<std::mutex> lock(m_pendingMutex);
                m_pendingRequests.erase(hash);   // ALWAYS release the slot
                if (!ok) m_failedChunks.insert(hash);
            }
        }));
    }

    void TerrainStreamingSystem::processLODUpdate(const LODUpdate& update) {
        // NOTA: la DESCARGA de GPU (chunksToUnload) ya NO se hace aquí. La gestiona
        // el PlanetarySystem de forma DIFERIDA: no quita el chunk viejo hasta que su
        // reemplazo de LOD (padre al fusionar, o los 4 hijos al subdividir) está
        // residente en GPU → evita el agujero/parpadeo al recargar y que los props
        // floten sobre terreno momentáneamente ausente.

        // 2. REFRESCAR en la Caché lo que se mantiene (para que el LRU no lo borre)
        for (const auto& key : update.chunksToKeep) {
            m_cache.getChunk(key); 
        }

        // 3. CARGAR lo nuevo a la GPU
        for (const auto& key : update.chunksToLoad) {
            // Copy out of the cache UNDER ITS LOCK (not a raw pointer): a concurrent async
            // addChunk (insert/rehash/evict) would otherwise dangle the pointer → crash.
            ChunkData data;
            if (m_cache.getChunkCopy(key, data))
                m_renderer.addToScene(update.planetName, key, data);
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