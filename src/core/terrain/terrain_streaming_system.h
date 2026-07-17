#pragma once

#include <queue>
#include <deque>
#include <future>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <thread>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include "core/chunk_cache.h"
#include "core/lod_system.h"
#include "core/terrain/terrain_generator.h"
#include "renderer/terrain_renderer.h"
#include "tools/planetary_types.h"

namespace Haruka {

    class TerrainStreamingSystem {
    public:
        TerrainStreamingSystem(ChunkCache& cache, TerrainGenerator& generator, TerrainRenderer& renderer)
            : m_cache(cache), m_generator(generator), m_renderer(renderer) {
            // Acota la concurrencia al nº de núcleos (×2 para solapar). Antes 0 = sin
            // límite → con el planeta entero, std::async lanzaba miles de hilos a la
            // vez (thrash). Generar cercanos primero (sort en PlanetarySystem) sin
            // sobre-suscribir la CPU es más rápido.
            unsigned hw = std::thread::hardware_concurrency();
            m_maxInFlight = (size_t)std::max(4u, hw ? hw * 2u : 8u);
            startPool();
        }

        ~TerrainStreamingSystem();

        /**
         * @brief Registra el conjunto de chunks deseados, ORDENADO de más cercano a
         *        más lejano al jugador. Reemplaza el set anterior y lanza el pump.
         *
         * El streaming no genera todo de golpe: mantiene esta cola y, cada frame,
         * despacha sólo los más cercanos que falten hasta el límite de concurrencia
         * (pump()). Así los chunks cercanos se generan/cargan primero y los lejanos
         * van entrando después, sin saturar de hilos.
         */
        void setDesiredChunks(std::vector<PlanetChunkKey> sortedNearToFar,
                              const nlohmann::json& planetSettings);

        /**
         * @brief Despacha generación de los chunks pendientes más cercanos hasta el
         *        presupuesto de concurrencia. Llamar cada frame (barato si no hay nada).
         */
        void pump();

        /**
         * @brief Aplica los cambios de LOD a la escena (Agregar/Quitar chunks).
         */
        void processLODUpdate(const LODUpdate& update);

        /** @brief (F7) Presupuesto de SUBIDAS a GPU para este frame. El PlanetarySystem
         *  lo resetea cada frame; processLODUpdate sube como mucho ese nº de chunks y deja
         *  el resto para frames siguientes → reparte la ráfaga de carga (sin picos de ms). */
        void setUploadBudget(int n) { m_uploadBudget = n; }
        /** @brief (NUEVO) Presupuesto de TIEMPO de pared para subidas este frame (ms).
         *  El presupuesto por COUNT no acota el pico: cada subida cuesta 2–3 ms (VAO+VBOs
         *  +malla de agua), así que 40 subidas = ~100 ms de stall (los picos de 112 ms del
         *  profiler). Time-box: cortamos las subidas al pasarnos de este presupuesto, suba
         *  lo que suba → el peor frame queda acotado y el resto entra en frames siguientes.
         *  Se debe llamar 1×/frame ANTES de las subidas para fijar el deadline. */
        void beginUploadFrame(int count, double msBudget) {
            m_uploadBudget   = count;
            m_uploadDeadline = std::chrono::steady_clock::now()
                             + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                   std::chrono::duration<double, std::milli>(msBudget));
            m_uploadTimeBoxed = true;
        }
        /** @brief Consume 1 del presupuesto de subida; false si agotado (por COUNT o por
         *  TIEMPO). Para que el agua del PlanetarySystem comparta el mismo presupuesto. */
        bool tryConsumeUpload() {
            if (m_uploadBudget <= 0) return false;
            if (m_uploadTimeBoxed && std::chrono::steady_clock::now() >= m_uploadDeadline) return false;
            --m_uploadBudget; return true;
        }

        /**
         * @brief Recupera los chunks que ya terminaron de generarse.
         */
        std::vector<std::shared_ptr<ChunkData>> getReadyChunks();

        int getPendingCount() const { return static_cast<int>(m_pendingRequests.size()); }
        // Chunks still WAITING in the desired queue (not yet dispatched async). The real
        // backlog: getPendingCount() is only the few in-flight, so loading screens must add this.
        int getQueuedCount()  const {
            std::lock_guard<std::mutex> lock(m_desiredMutex);
            size_t n = 0; for (auto& [k, d] : m_desiredByPlanet) n += d.chunks.size();
            return static_cast<int>(n);
        }


    private:
        ChunkCache& m_cache;
        TerrainGenerator& m_generator;
        TerrainRenderer& m_renderer;

        // Chunks que están siendo generados actualmente
        std::unordered_set<uint64_t> m_pendingRequests;
        std::mutex m_pendingMutex;

        // --- POOL de trabajadores (generación CPU + ensamblado de malla del camino GPU) ----------
        // Antes cada chunk se lanzaba con std::async(launch::async) = UN HILO NUEVO POR CHUNK.
        // Con decenas de chunks por frame, crear/destruir hilos costaba más que el trabajo en sí
        // (y el vector de futures había que barrerlo cada frame: `pump.reap`). Ahora: N hilos fijos
        // y una cola. `m_pendingRequests` sigue siendo el que limita el trabajo en vuelo.
        std::vector<std::thread>          m_pool;
        std::deque<std::function<void()>> m_jobs;
        std::mutex                        m_jobMutex;
        std::condition_variable           m_jobCv;
        bool                              m_poolStop = false;
        void startPool();
        void submit(std::function<void()> job, bool urgent = false);

        // Cola de deseo POR PLANETA (cercano→lejano) + ajustes, para el pump por frame.
        // CLAVE: con varios planetas, un único set se sobreescribía entre planetas y
        // pump() solo generaba el ÚLTIMO → la Tierra (cientos de chunks) se quedaba sin
        // generar. Ahora cada planeta conserva su set y pump() los recorre todos.
        struct DesiredSet {
            std::vector<PlanetChunkKey> chunks;
            nlohmann::json              settings;
            std::string                 planetName;
        };
        std::unordered_map<std::string, DesiredSet> m_desiredByPlanet;
        mutable std::mutex m_desiredMutex;
        // Chunks cuya generación lanzó excepción → no reintentar (evita spin + stall).
        std::unordered_set<uint64_t> m_failedChunks; // protegido por m_pendingMutex
        // Máximo de generaciones en vuelo a la vez (cercanos primero). 0 = sin límite.
        // Por defecto SIN límite: igual que el streaming original (sin huecos); el
        // orden cercano→lejano lo da el sort en PlanetarySystem. Subir >0 sólo si se
        // quiere limitar hilos (con el erase exception-safe ya no puede causar stall).
        size_t m_maxInFlight = 0;
        int    m_uploadBudget = 1000000; // F7: subidas a GPU restantes este frame (sin cap si no se resetea)
        std::chrono::steady_clock::time_point m_uploadDeadline{}; // time-box de subidas (ver beginUploadFrame)
        bool   m_uploadTimeBoxed = false;                        // el time-box sólo aplica si se llamó beginUploadFrame
        
        // Resultados listos para ser inyectados en la escena
        std::vector<std::shared_ptr<ChunkData>> m_completedChunks;
        std::mutex m_resultMutex;

        // Trabajos GPU (compute) en vuelo: dispatch asíncrono (fence), se cosechan
        // cuando terminan. Solo hilo principal.
        struct GpuJob { int slot; PlanetChunkKey key; nlohmann::json settings; std::string planet; double radius; };
        std::vector<GpuJob> m_gpuJobs;
        void harvestGpuJobs(); // cosecha los terminados → cache + completados

        void requestAsyncGeneration(const PlanetChunkKey& key, const nlohmann::json& settings, const std::string& planetName);
        // Genera SÍNCRONO en el hilo principal (necesario para el camino GPU/compute).
        void generateSyncMainThread(const PlanetChunkKey& key, const nlohmann::json& settings, const std::string& planetName);
    };

}