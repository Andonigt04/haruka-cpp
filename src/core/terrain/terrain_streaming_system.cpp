#include "terrain_streaming_system.h"

#include "lod_system.h" // Para LODUpdate
#include "renderer/terrain_renderer.h" // Para agregar/quitar de la escena

namespace Haruka {
    void TerrainStreamingSystem::update(const std::vector<PlanetChunkKey>& requestedChunks, const nlohmann::json& planetSettings) {
        for (const auto& key : requestedChunks) {
            uint64_t hash = ChunkCache::keyToHash(key); // Usamos tu función de hash

            // 1. ¿Está en caché?
            if (m_cache.hasChunk(key)) {
                continue; // Ya está listo, no hacemos nada
            }

            // 2. ¿Ya lo estamos generando?
            if (m_pendingRequests.find(hash) != m_pendingRequests.end()) {
                continue; 
            }

            // 3. No está. Pedir generación asíncrona.
            requestAsyncGeneration(key, planetSettings);
        }
    }

    void TerrainStreamingSystem::requestAsyncGeneration(const PlanetChunkKey& key, const nlohmann::json& settings) {
        uint64_t hash = ChunkCache::keyToHash(key);
        m_pendingRequests.insert(hash);

        // Lanzar hilo de trabajo
        std::async(std::launch::async, [this, key, settings, hash]() {
            // Ejecutar el generador (Ruido Perlin/fBm)
            auto data = m_generator.generateChunk(key, settings, settings["radius"]);

            // Guardar resultado de forma segura
            std::lock_guard<std::mutex> lock(m_resultMutex);
            m_cache.addChunk(key, *data); // Guardar en caché para la próxima vez
            m_completedChunks.push_back(data);
            
            // El hash se eliminaría de pending en el hilo principal para evitar race conditions
        });
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
            } else {
                // ¡MISS! No está en RAM, hay que generarlo en un hilo
                requestAsyncGeneration(key, update.planetName);
            }
        }
    }
}