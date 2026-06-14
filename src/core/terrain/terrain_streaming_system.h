#pragma once

#include <queue>
#include <future>
#include <mutex>
#include <chrono>
#include <thread>
#include <algorithm>
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

        /**
         * @brief Recupera los chunks que ya terminaron de generarse.
         */
        std::vector<std::shared_ptr<ChunkData>> getReadyChunks();

        int getPendingCount() const { return static_cast<int>(m_pendingRequests.size()); }
        // Chunks still WAITING in the desired queue (not yet dispatched async). The real
        // backlog: getPendingCount() is only the few in-flight, so loading screens must add this.
        int getQueuedCount()  const { return static_cast<int>(m_desiredChunks.size()); }


    private:
        ChunkCache& m_cache;
        TerrainGenerator& m_generator;
        TerrainRenderer& m_renderer;

        // Chunks que están siendo generados actualmente
        std::unordered_set<uint64_t> m_pendingRequests;
        std::mutex m_pendingMutex;
        std::vector<std::future<void>> m_asyncTasks;

        // Cola de deseo ordenada (cercano→lejano) + ajustes, para el pump por frame.
        std::vector<PlanetChunkKey> m_desiredChunks;
        nlohmann::json m_desiredSettings;
        std::string m_desiredPlanetName;
        std::mutex m_desiredMutex;
        // Chunks cuya generación lanzó excepción → no reintentar (evita spin + stall).
        std::unordered_set<uint64_t> m_failedChunks; // protegido por m_pendingMutex
        // Máximo de generaciones en vuelo a la vez (cercanos primero). 0 = sin límite.
        // Por defecto SIN límite: igual que el streaming original (sin huecos); el
        // orden cercano→lejano lo da el sort en PlanetarySystem. Subir >0 sólo si se
        // quiere limitar hilos (con el erase exception-safe ya no puede causar stall).
        size_t m_maxInFlight = 0;
        
        // Resultados listos para ser inyectados en la escena
        std::vector<std::shared_ptr<ChunkData>> m_completedChunks;
        std::mutex m_resultMutex;

        void requestAsyncGeneration(const PlanetChunkKey& key, const nlohmann::json& settings, const std::string& planetName);
        void reapFinishedTasks();
    };

}