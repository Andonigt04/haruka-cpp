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
        std::string name = planetSettings.value("planetName", std::string{});
        {
            std::lock_guard<std::mutex> lock(m_desiredMutex);
            auto& d = m_desiredByPlanet[name];
            d.chunks     = std::move(sortedNearToFar);
            d.settings   = planetSettings;
            d.planetName = name;
        }
        pump();
    }

    void TerrainStreamingSystem::pump() {
        reapFinishedTasks();
        harvestGpuJobs(); // SIEMPRE: cosecha los chunks GPU terminados (aunque no haya nuevos)

        std::lock_guard<std::mutex> dlock(m_desiredMutex);

        // Recorre el set deseado de CADA planeta (no solo el último). El presupuesto
        // de concurrencia (m_maxInFlight) y los slots GPU son COMPARTIDOS; al llenarse
        // cortamos el pump entero. Cada lista ya viene cercano→lejano, así que el
        // planeta donde está la cámara (sus chunks más cercanos) entra primero.
        for (auto& [name, d] : m_desiredByPlanet) {
            if (d.chunks.empty()) continue;

            // Camino GPU (opt-in por planeta): el cómputo (compute) corre en el hilo
            // principal (pump) de forma ASÍNCRONA (fence) → varios chunks en vuelo sin
            // bloquear el frame. Si está off → async CPU en workers como siempre.
            const auto& cfg = d.settings.contains("config") ? d.settings["config"] : d.settings;
            const bool gpu = cfg.value("gpuTerrain", false);
            const double radius = d.settings.value("radius", 1.0);

            for (const auto& key : d.chunks) { // ya viene cercano→lejano
                uint64_t hash = ChunkCache::keyToHash(key);
                if (m_cache.hasChunk(key)) continue; // ya generado en RAM
                {
                    std::lock_guard<std::mutex> lock(m_pendingMutex);
                    if (m_pendingRequests.count(hash)) continue; // ya en vuelo (CPU o GPU)
                    if (m_failedChunks.count(hash))    continue; // generación falló antes → no reintentar en bucle
                }

                if (gpu) {
                    if (!m_generator.gpuHasFreeSlot()) break; // sin slots GPU → siguiente planeta
                    int slot = m_generator.gpuDispatch(key, d.settings, radius);
                    if (slot >= 0) {
                        { std::lock_guard<std::mutex> lock(m_pendingMutex); m_pendingRequests.insert(hash); }
                        m_gpuJobs.push_back({ slot, key, d.settings, d.planetName, radius });
                        continue;
                    }
                    // No elegible para GPU (perfil no-terran, etc.) → cae a async CPU.
                }

                // Respetar el presupuesto de concurrencia async (cercanos primero).
                // Al llenarse, NO hay más slots para NINGÚN planeta → corta el pump.
                {
                    std::lock_guard<std::mutex> lock(m_pendingMutex);
                    if (m_maxInFlight != 0 && m_pendingRequests.size() >= m_maxInFlight) return;
                }
                requestAsyncGeneration(key, d.settings, d.planetName);
            }
        }
    }

    void TerrainStreamingSystem::harvestGpuJobs() {
        for (auto it = m_gpuJobs.begin(); it != m_gpuJobs.end();) {
            // Lee elev+normal+agua del GPU (rápido, no bloquea: solo si el fence terminó).
            auto elev  = std::make_shared<std::vector<float>>();
            auto norm  = std::make_shared<std::vector<glm::vec3>>();
            auto water = std::make_shared<std::vector<float>>();
            if (!m_generator.gpuHarvestData(it->slot, *elev, *norm, *water)) { ++it; continue; } // aún computa

            // ENSAMBLA la malla en un WORKER (CPU, sin GL) → no clava el hilo principal.
            PlanetChunkKey key = it->key; nlohmann::json settings = it->settings;
            std::string planet = it->planet; double radius = it->radius;
            uint64_t hash = ChunkCache::keyToHash(key);
            m_asyncTasks.push_back(std::async(std::launch::async, [this, key, settings, planet, radius, elev, norm, water, hash]() {
                bool ok = false;
                try {
                    auto data = m_generator.generateChunk(key, settings, radius, false, elev.get(), norm.get(), water.get());
                    data->key = key; data->planetName = planet;
                    m_cache.addChunk(key, *data);
                    { std::lock_guard<std::mutex> lock(m_resultMutex); m_completedChunks.push_back(data); }
                    ok = true;
                } catch (const std::exception& e) {
                    fprintf(stderr, "[stream] GPU-assembly FAILED face=%d lod=%d x=%u y=%u: %s\n",
                            (int)key.face, (int)key.lod, key.x, key.y, e.what());
                } catch (...) {
                    fprintf(stderr, "[stream] GPU-assembly FAILED face=%d lod=%d x=%u y=%u: (desconocido)\n",
                            (int)key.face, (int)key.lod, key.x, key.y);
                }
                std::lock_guard<std::mutex> lock(m_pendingMutex);
                m_pendingRequests.erase(hash);
                if (!ok) m_failedChunks.insert(hash);
            }));
            it = m_gpuJobs.erase(it);
        }
    }

    void TerrainStreamingSystem::generateSyncMainThread(const PlanetChunkKey& key,
                                                        const nlohmann::json& settings,
                                                        const std::string& planetName) {
        uint64_t hash = ChunkCache::keyToHash(key);
        try {
            auto data = m_generator.generateChunk(key, settings, settings["radius"], /*mainThread*/true);
            data->key        = key;
            data->planetName = planetName;
            m_cache.addChunk(key, *data);
            std::lock_guard<std::mutex> lock(m_resultMutex);
            m_completedChunks.push_back(data);
        } catch (...) {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            m_failedChunks.insert(hash);
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
            } catch (const std::exception& e) {
                fprintf(stderr, "[stream] CPU gen FAILED face=%d lod=%d x=%u y=%u: %s\n",
                        (int)key.face, (int)key.lod, key.x, key.y, e.what());
            } catch (...) {
                fprintf(stderr, "[stream] CPU gen FAILED face=%d lod=%d x=%u y=%u: (desconocido)\n",
                        (int)key.face, (int)key.lod, key.x, key.y);
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

        // 2. KEEP: refresca la caché (LRU) y, CLAVE, SUBE a GPU lo que esté cacheado
        //    pero aún no residente. Sin esto, un chunk que se generó mientras la
        //    cámara se movía (llegó cuando ya no era "visible" → no se subió) pasa a
        //    chunksToKeep y NUNCA se sube → queda en caché pero no en GPU (lo que
        //    pasaba: 327 cacheados, 6 en GPU). addToScene es idempotente (salta los
        //    ya residentes), así que esto solo sube los que faltan.
        // PRESUPUESTO DE FRAME (F7): subir un chunk = copia pesada (getChunkCopy ~150 KB)
        // + creación de buffers GPU. En una ráfaga (carga inicial / fly-in) había CIENTOS
        // por frame → pico de >1500 ms. Capamos cuántos se suben por frame; los que faltan
        // se suben en frames siguientes (processLODUpdate se vuelve a llamar). isResident
        // refresca gratis (no consume presupuesto), así que lo ya subido no cuenta.
        for (const auto& key : update.chunksToKeep) {
            if (m_renderer.isResident(key)) { m_cache.getChunk(key); continue; } // ya en GPU: solo refresca LRU
            if (!tryConsumeUpload()) return;                                      // presupuesto (count o tiempo) agotado → el resto, otro frame
            ChunkData data;
            if (m_cache.getChunkCopy(key, data)) {
                m_renderer.addToScene(update.planetName, key, data); // sube el que falta
            } else {
                ++m_uploadBudget; // no se subió nada (no estaba en caché) → devuelve el crédito
            }
        }

        // 3. CARGAR lo nuevo a la GPU
        for (const auto& key : update.chunksToLoad) {
            if (m_renderer.isResident(key)) continue;       // ya subido (idempotente): gratis
            if (!tryConsumeUpload()) return;                // presupuesto (count o tiempo) agotado
            // Copy out of the cache UNDER ITS LOCK (not a raw pointer): a concurrent async
            // addChunk (insert/rehash/evict) would otherwise dangle the pointer → crash.
            ChunkData data;
            if (m_cache.getChunkCopy(key, data)) {
                m_renderer.addToScene(update.planetName, key, data);
            } else {
                ++m_uploadBudget; // no se subió nada (no estaba en caché) → devuelve el crédito
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