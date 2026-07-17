#include "terrain_streaming_system.h"

#include "core/lod_system.h" // Para LODUpdate
#include "renderer/terrain_renderer.h" // Para agregar/quitar de la escena
#include "tools/profiler.h" // sub-scopes de pump (diagnóstico del cuello lod.pump)

namespace Haruka {
    void TerrainStreamingSystem::startPool() {
        // TANTOS HILOS COMO TRABAJO EN VUELO PERMITIMOS (m_maxInFlight). Con un pool más pequeño,
        // los chunks de CPU (lentos) ocupan todos los hilos y los trabajos de ENSAMBLADO GPU se
        // quedan en cola — y esos RETIENEN UN SLOT GPU hasta que corren → el streaming se para en
        // seco (síntoma: el conteo de chunks se congela y nunca se asienta). El límite real de
        // trabajo concurrente es m_maxInFlight, igual que antes; lo que se elimina es el
        // crear/destruir un hilo por chunk.
        const unsigned n = (unsigned)std::max<size_t>(4, m_maxInFlight);
        for (unsigned i = 0; i < n; ++i) {
            m_pool.emplace_back([this]() {
                for (;;) {
                    std::function<void()> job;
                    {
                        std::unique_lock<std::mutex> lock(m_jobMutex);
                        m_jobCv.wait(lock, [this] { return m_poolStop || !m_jobs.empty(); });
                        if (m_poolStop && m_jobs.empty()) return;
                        job = std::move(m_jobs.front());
                        m_jobs.pop_front();
                    }
                    job(); // el job ya captura y trata sus excepciones
                }
            });
        }
    }

    void TerrainStreamingSystem::submit(std::function<void()> job, bool urgent) {
        {
            std::lock_guard<std::mutex> lock(m_jobMutex);
            // `urgent` = el trabajo RETIENE UN SLOT GPU (los 16 son un recurso escaso): va delante
            // de las generaciones por CPU, que no bloquean a nadie mientras esperan.
            if (urgent) m_jobs.push_front(std::move(job));
            else        m_jobs.push_back(std::move(job));
        }
        m_jobCv.notify_one();
    }

    TerrainStreamingSystem::~TerrainStreamingSystem() {
        { std::lock_guard<std::mutex> lock(m_jobMutex); m_poolStop = true; }
        m_jobCv.notify_all();
        for (auto& t : m_pool) if (t.joinable()) t.join();
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
        { HARUKA_PROFILE("pump.harvest"); harvestGpuJobs(); } // cosecha chunks GPU terminados

        HARUKA_PROFILE("pump.scan"); // escaneo del set deseado + dispatch de generación
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

            int diskBudget = 16;   // chunks leídos de disco por pasada (ver nota abajo)
            // PRESUPUESTO DE ESPERAS POR MESO. Un chunk cuya tesela aún no está devuelve -2 y se
            // REINTENTA. Sin cota, cada frame se vuelven a sondear TODOS los que esperan (cada sondeo
            // = 5 requestTile con lock) y además siguen en la lista de deseados → `pump.scan` y
            // `stream.sort` crecen con el atasco (medido: p99 de 10 ms cada uno al encender el meso).
            // El worker construye teselas a un ritmo fijo, así que sondear más no acelera NADA: solo
            // cuesta. Con la cota, el resto se reintenta en frames siguientes; mientras, el ancestro
            // grueso cubre la zona → sin agujeros, igual que antes.
            int mesoWaitBudget = 24;
            for (const auto& key : d.chunks) { // ya viene cercano→lejano
                uint64_t hash = ChunkCache::keyToHash(key);
                if (m_cache.hasChunk(key)) continue; // ya generado en RAM

                // CACHÉ EN DISCO: antes de REGENERAR (compute + erosión + readback: ~5-7 ms), mirar
                // si ya lo calculamos alguna vez. Leer cuesta ~0.5 ms → mata el thrashing.
                //
                // ⚠️ CON PRESUPUESTO POR PASADA. Sin él, leer es TAN rápido que entran cientos de
                // chunks de golpe, se salta el ritmo con el que el LOD hace su cascada grueso→fino, y
                // el streaming NO SE ASIENTA (3 tests en rojo). El disco es una vía rápida, no una
                // barra libre: se le aplica el MISMO caudal que a la generación.
                if (diskBudget > 0) {
                    ChunkData disk;
                    if (m_cache.loadFromDisk(key, disk)) {
                        m_cache.addChunk(key, disk);
                        --diskBudget;
                        continue;
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(m_pendingMutex);
                    if (m_pendingRequests.count(hash)) continue; // ya en vuelo (CPU o GPU)
                    if (m_failedChunks.count(hash))    continue; // generación falló antes → no reintentar en bucle
                }

                if (gpu) {
                    if (!m_generator.gpuHasFreeSlot()) break; // sin slots GPU → siguiente planeta
                    HARUKA_PROFILE("scan.dispatch");
                    int slot = m_generator.gpuDispatch(key, d.settings, radius);
                    // -2 = la tesela MESO de este chunk aún no está (se ha encolado al worker). NO
                    // generamos con datos incompletos ni caemos a CPU: reintentamos en otro frame.
                    // Mientras, el ancestro grueso cubre la zona (ya no se evicta) → sin agujeros.
                    if (slot == -2) {                 // tesela no lista: encolada, se reintentará
                        if (--mesoWaitBudget <= 0) break;  // ya hay bastantes en cola → siguiente planeta
                        continue;
                    }
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
            // El hilo de RENDER solo comprueba el fence y coge los punteros mapeados: no copia nada.
            // Los buffers de readback están en memoria NO CACHEADA (leerlos va a cientos de MB/s),
            // así que copiar aquí 16 chunks de golpe costaba varios ms de frame. La copia la hace
            // el worker; el slot GPU queda reservado hasta que la termina (gpuReleaseSlot).
            TerrainGenerator::GpuMappedView view;
            const int slot = it->slot;
            if (!m_generator.gpuMapHarvest(slot, view)) { ++it; continue; } // aún computa

            // ENSAMBLA la malla en un WORKER (CPU, sin GL) → no clava el hilo principal.
            PlanetChunkKey key = it->key; nlohmann::json settings = it->settings;
            std::string planet = it->planet; double radius = it->radius;
            uint64_t hash = ChunkCache::keyToHash(key);
            submit([this, key, settings, planet, radius, view, slot, hash]() {
                bool ok = false;
                // El slot se suelta EXACTAMENTE UNA VEZ. Soltarlo dos veces es catastrófico: en
                // cuanto otro chunk lo reutiliza, el segundo release lo marca libre ESTANDO EN USO
                // → se despachan dos trabajos al mismo slot y ninguno se cosecha jamás (síntoma:
                // el conteo de chunks se congela y el streaming no se asienta).
                struct SlotGuard {
                    TerrainGenerator& gen; int slot; bool held = true;
                    void release() { if (held) { held = false; gen.gpuReleaseSlot(slot); } }
                    ~SlotGuard() { release(); } // si la copia lanza (bad_alloc), no se filtra
                } guard{ m_generator, slot };
                try {
                    // Copia desde la memoria mapeada (lo caro) — ya fuera del hilo de render.
                    std::vector<float>     elev(view.elev,  view.elev  + view.count);
                    std::vector<float>     water(view.water, view.water + view.count);
                    std::vector<glm::vec3> norm(view.count);
                    for (size_t i = 0; i < view.count; ++i) norm[i] = glm::vec3(view.norm4[i]);
                    guard.release(); // los datos ya son nuestros → suelta el slot

                    auto data = m_generator.generateChunk(key, settings, radius, false, &elev, &norm, &water);
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
            }, /*urgent*/true); // retiene un slot GPU → delante de las generaciones por CPU
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

        submit([this, key, settings, hash, planetName]() {
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
        });
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
        // SNAPSHOT de residencia: isResident() toma el mutex del renderer, y aquí se preguntaba por
        // CADA chunk (~1500) → 1500 lock/unlock por frame, más otros tantos en la caché para refrescar
        // el LRU. Una copia bajo un solo lock + refresco por lotes hace el mismo trabajo sin la
        // contención (era el grueso de `stream.processLOD`).
        static std::unordered_set<uint64_t> s_resident; // reusa el buffer entre frames (hilo de render)
        m_renderer.residentHashes(s_resident);
        auto resident = [](const PlanetChunkKey& k) { return s_resident.count(ChunkCache::keyToHash(k)) != 0; };

        static std::vector<PlanetChunkKey> s_touch;
        s_touch.clear();
        for (const auto& key : update.chunksToKeep) {
            if (resident(key)) { s_touch.push_back(key); continue; } // ya en GPU: solo refresca LRU (en lote)
            if (!tryConsumeUpload()) break;                          // presupuesto agotado → el resto, otro frame
            ChunkData data;
            if (m_cache.getChunkCopy(key, data)) {
                m_renderer.addToScene(update.planetName, key, data); // sube el que falta
            } else {
                ++m_uploadBudget; // no se subió nada (no estaba en caché) → devuelve el crédito
            }
        }
        m_cache.touchMany(s_touch); // un solo lock para los ~1500 refrescos de LRU

        // 3. CARGAR lo nuevo a la GPU
        for (const auto& key : update.chunksToLoad) {
            if (resident(key)) continue;                    // ya subido (idempotente): gratis
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
        std::lock_guard<std::mutex> lock(m_resultMutex);
        std::vector<std::shared_ptr<ChunkData>> ready;
        ready.swap(m_completedChunks);
        return ready;
    }
}