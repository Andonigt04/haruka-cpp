/**
 * @file chunk_cache.h
 * @brief LRU cache for terrain chunks with configurable memory limits.
 */
#pragma once
#include <deque>
#include <thread>
#include <condition_variable>
#include <atomic>

#include "tools/planetary_types.h"
#include <unordered_map>
#include <list>
#include <memory>
#include <mutex>
#include <cstddef>

namespace Haruka {

/**
 * @brief Least Recently Used (LRU) cache for terrain chunks.
 * 
 * Maintains a fixed-size cache of recently generated chunks. When the cache
 * is full, the least recently used chunk is evicted to make room for new ones.
 * 
 * This is useful for avoiding re-generation of chunks that were recently
 * accessed but are temporarily out of view.
 */
class ChunkCache {
public:
    /// LOD hasta el que la pirámide de chunks queda PINNEADA (nunca se evicta). Es el fallback del
    /// render: sin ancestro residente, un chunk fino que no llega deja un AGUJERO. 6 caras × 4^lod
    /// → con 5 son ~6k chunks gruesos, baratos y siempre disponibles.
    static constexpr uint8_t kPinnedLOD = 5;

private:
    std::string m_diskDir;      // vacío = sin caché de disco

    // ESCRITOR EN SEGUNDO PLANO. Escribir ~500 KB por chunk DENTRO de addChunk (que corre en el
    // camino de cosecha, `pump.harvest`) hundía el frame: I/O síncrono en el hilo caliente. Ahora se
    // encola y escribe un hilo aparte. La cola está ACOTADA y se descarta lo que sobra: persistir es
    // una optimización, no una obligación — perder una escritura solo cuesta regenerar ese chunk.
    struct DiskJob { PlanetChunkKey key; ChunkData data; };
    std::deque<DiskJob>     m_diskQueue;
    mutable std::mutex      m_diskMx;
    std::condition_variable m_diskCv;
    std::thread             m_diskThread;
    std::atomic<bool>       m_diskStop{false};
    void diskWriterLoop();
    void enqueueDiskWrite(const PlanetChunkKey& key, const ChunkData& d);
public:

    /**
     * @brief Initializes the chunk cache with specified memory limit.
     * @param maxMemoryMB Maximum memory to use for cached chunks (MB)
     */
    explicit ChunkCache(size_t maxMemoryMB = 128);
    
    ~ChunkCache();   // para el hilo escritor de disco
    
    /**
     * @brief Converts a PlanetChunkKey to a uint64_t hash.
     */
    static uint64_t keyToHash(const PlanetChunkKey& key);

    /**
     * @brief Retrieves a cached chunk if available.
     * @param key Chunk identifier
     * @return Pointer to cached ChunkData, or nullptr if not in cache
     */
    const ChunkData* getChunk(const PlanetChunkKey& key);

    /** @brief Refresca el LRU de MUCHOS chunks con UN solo lock.
     *  El streaming refrescaba chunk a chunk (getChunk) los ~1500 chunks que mantiene visibles:
     *  1500 lock/unlock por frame. Es el mismo trabajo, pero pagando el mutex una vez. */
    void touchMany(const std::vector<PlanetChunkKey>& keys);

    /**
     * @brief Copies a cached chunk into `out` while holding the cache lock.
     * @return true if found. Use this (not getChunk) when the data is consumed AFTER the
     *  call: getChunk returns a pointer INTO the map, which a concurrent addChunk
     *  (async generation → insert/rehash/evict) can invalidate → use-after-free.
     */
    bool getChunkCopy(const PlanetChunkKey& key, ChunkData& out);
    
    /**
     * @brief Adds a chunk to the cache.
     * 
     * If the cache exceeds the memory limit, least recently used chunks are evicted.
     * 
     * @param key Chunk identifier
     * @param data Chunk data to cache
     */
    void addChunk(const PlanetChunkKey& key, const ChunkData& data);
    
    /**
     * @brief Removes a chunk from the cache.
     * @param key Chunk identifier
     */
    void removeChunk(const PlanetChunkKey& key);
    
    /**
     * @brief Checks if a chunk exists in the cache.
     * @param key Chunk identifier
     * @return true if chunk is cached, false otherwise
     */
    bool hasChunk(const PlanetChunkKey& key) const;

    /**
     * @brief CACHÉ EN DISCO. Hoy, evictar un chunk = **recalcularlo desde cero** la próxima vez, y
     * generar es caro (compute + erosión + readback: ~5-7 ms). Con la caché de RAM al tope, eso es
     * thrashing puro: el terreno desaparece y reaparece mientras se regenera.
     *
     * Con esto, evictar = ESCRIBIR y fallar = LEER. La regeneración pasa de recomputar a cargar.
     * El directorio se separa por SEED: un mundo nuevo no lee los chunks del anterior.
     */
    /// ⚠️ SUBIR ESTE NÚMERO SIEMPRE QUE CAMBIE LA GENERACIÓN DEL TERRENO (fórmula, campos, erosión).
    /// La caché se separa por seed... pero eso NO basta: si cambias la generación, los chunks viejos
    /// son de un mundo que YA NO EXISTE y se seguirían sirviendo → terreno incoherente, huecos y el
    /// LOD sin asentarse (pasó: 3 tests en rojo hasta borrar la caché a mano).
    static constexpr uint32_t kGenVersion = 3;   // v3: tectónica + campos erosionados + orogenia horneada
    void setDiskCache(const std::string& dir, uint32_t seed);
    /** @brief Intenta cargar el chunk del disco (sin generarlo). */
    bool loadFromDisk(const PlanetChunkKey& key, ChunkData& out) const;
    /** @brief Escribe el chunk al disco (lo llama la evicción). */
    void saveToDisk(const PlanetChunkKey& key, const ChunkData& data) const;
    
    /**
     * @brief Clears all cached chunks.
     */
    void clear();
    
    /**
     * @brief Returns the number of cached chunks.
     */
    size_t getChunkCount() const { return cache.size(); }
    
    /**
     * @brief Returns current memory usage in MB.
     */
    size_t getMemoryUsageMB() const { return currentMemoryBytes / (1024 * 1024); }
    
    /**
     * @brief Returns the memory limit in MB.
     */
    size_t getMaxMemoryMB() const { return maxMemoryBytes / (1024 * 1024); }
    
    /**
     * @brief Sets a new memory limit.
     * 
     * If the new limit is smaller than current usage, chunks are evicted
     * in LRU order until the usage is within limits.
     * 
     * @param newMaxMemoryMB New maximum memory in MB
     */
    void setMaxMemory(size_t newMaxMemoryMB);
    
    /**
     * @brief Returns cache hit/miss statistics.
     */
    struct CacheStats {
        size_t hits = 0;
        size_t misses = 0;
        size_t evictions = 0;
        
        float hitRate() const {
            size_t total = hits + misses;
            return (total == 0) ? 0.0f : static_cast<float>(hits) / total;
        }
    };
    
    const CacheStats& getStats() const { return stats; }
    void resetStats() { stats = CacheStats{}; }
    
private:
    /**
     * @brief Cache entry with metadata.
     */
    struct CacheEntry {
        ChunkData data;
        size_t sizeBytes = 0;
        // List iterator for LRU tracking (stored in separate member)
    };
    
    // Main cache storage: key -> (data, size)
    std::unordered_map<uint64_t, CacheEntry> cache;
    
    // LRU ordering: list of keys in access order (least recent to most recent)
    // We use a custom hash for PlanetChunkKey to use unordered_map
    struct ChunkKeyHash {
        size_t operator()(const PlanetChunkKey& key) const {
            return static_cast<size_t>(ChunkCache::keyToHash(key)); // empaquetado exacto (incluye body)
        }
    };

    struct ChunkKeyEqual {
        bool operator()(const PlanetChunkKey& a, const PlanetChunkKey& b) const {
            return a == b; // operator== incluye body
        }
    };
    
    std::list<PlanetChunkKey> lruOrder;  ///< LRU ordering (front = oldest)
    std::unordered_map<uint64_t, std::list<PlanetChunkKey>::iterator> keyToIterator;  ///< Map to iterators
    
    mutable std::mutex m_mutex;
    size_t maxMemoryBytes;
    size_t currentMemoryBytes = 0;
    CacheStats stats;
    
    /**
     * @brief Evicts the least recently used chunk.
     * @return true if a chunk was evicted, false if cache is empty
     */
    bool evictLRU();
    
    /**
     * @brief Evicts chunks until memory usage is within limits.
     */
    void evictToFitMemory();
    
    /**
     * @brief Updates the LRU order for a recently accessed chunk.
     */
    void updateLRUOrder(const PlanetChunkKey& key);
};

} // namespace Haruka
