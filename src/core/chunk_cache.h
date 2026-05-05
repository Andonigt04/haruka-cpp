/**
 * @file chunk_cache.h
 * @brief LRU cache for terrain chunks with configurable memory limits.
 */
#pragma once

#include "core/world_system.h"
#include "game/planetary_system.h"
#include <unordered_map>
#include <list>
#include <memory>
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
    /**
     * @brief Initializes the chunk cache with specified memory limit.
     * @param maxMemoryMB Maximum memory to use for cached chunks (MB)
     */
    explicit ChunkCache(size_t maxMemoryMB = 128);
    
    ~ChunkCache() = default;
    
    /**
     * @brief Converts a PlanetChunkKey to a uint64_t hash.
     */
    static uint64_t keyToHash(const PlanetChunkKey& key);

    /**
     * @brief Retrieves a cached chunk if available.
     * @param key Chunk identifier
     * @return Pointer to cached ChunkData, or nullptr if not in cache
     */
    const PlanetarySystem::ChunkData* getChunk(const PlanetChunkKey& key);
    
    /**
     * @brief Adds a chunk to the cache.
     * 
     * If the cache exceeds the memory limit, least recently used chunks are evicted.
     * 
     * @param key Chunk identifier
     * @param data Chunk data to cache
     */
    void addChunk(const PlanetChunkKey& key, const PlanetarySystem::ChunkData& data);
    
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
        PlanetarySystem::ChunkData data;
        size_t sizeBytes = 0;
        // List iterator for LRU tracking (stored in separate member)
    };
    
    // Main cache storage: key -> (data, size)
    std::unordered_map<uint64_t, CacheEntry> cache;
    
    // LRU ordering: list of keys in access order (least recent to most recent)
    // We use a custom hash for PlanetChunkKey to use unordered_map
    struct ChunkKeyHash {
        size_t operator()(const PlanetChunkKey& key) const {
            // Combine face, lod, x, y into a single hash
            return ((static_cast<size_t>(key.face) << 24) |
                    (static_cast<size_t>(key.lod) << 16) |
                    (static_cast<size_t>(key.x) << 8) |
                    (static_cast<size_t>(key.y)));
        }
    };
    
    struct ChunkKeyEqual {
        bool operator()(const PlanetChunkKey& a, const PlanetChunkKey& b) const {
            return a.face == b.face && a.lod == b.lod && a.x == b.x && a.y == b.y;
        }
    };
    
    std::list<PlanetChunkKey> lruOrder;  ///< LRU ordering (front = oldest)
    std::unordered_map<uint64_t, std::list<PlanetChunkKey>::iterator> keyToIterator;  ///< Map to iterators
    
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
