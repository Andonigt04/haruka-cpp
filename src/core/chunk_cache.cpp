/**
 * @file chunk_cache.cpp
 * @brief Implementation of LRU chunk cache.
 */

#include "chunk_cache.h"
#include <algorithm>

namespace Haruka {

ChunkCache::ChunkCache(size_t maxMemoryMB)
    : maxMemoryBytes(std::max<size_t>(16 * 1024 * 1024, maxMemoryMB * 1024 * 1024)),
      currentMemoryBytes(0) {}

const ChunkData* ChunkCache::getChunk(const PlanetChunkKey& key) {
    uint64_t hash = keyToHash(key);
    auto it = cache.find(hash);
    
    if (it != cache.end()) {
        // Cache hit
        stats.hits++;
        updateLRUOrder(key);
        return &it->second.data;
    }
    
    // Cache miss
    stats.misses++;
    return nullptr;
}

void ChunkCache::addChunk(const PlanetChunkKey& key, const ChunkData& data) {
    uint64_t hash = keyToHash(key);
    
    // Calculate size of chunk data
    size_t chunkSizeBytes = 0;
    chunkSizeBytes += data.vertices.size() * sizeof(glm::vec3);
    chunkSizeBytes += data.normals.size() * sizeof(glm::vec3);
    chunkSizeBytes += data.colors.size() * sizeof(glm::vec3);
    chunkSizeBytes += data.indices.size() * sizeof(unsigned int);
    
    // Check if already cached
    auto existing = cache.find(hash);
    if (existing != cache.end()) {
        // Update existing entry
        currentMemoryBytes -= existing->second.sizeBytes;
        existing->second.data = data;
        existing->second.sizeBytes = chunkSizeBytes;
        currentMemoryBytes += chunkSizeBytes;
        updateLRUOrder(key);
    } else {
        // Add new entry
        CacheEntry entry{data, chunkSizeBytes};
        cache[hash] = entry;
        lruOrder.push_back(key);
        keyToIterator[hash] = std::prev(lruOrder.end());
        currentMemoryBytes += chunkSizeBytes;
    }
    
    // Evict chunks if memory limit exceeded
    evictToFitMemory();
}

void ChunkCache::removeChunk(const PlanetChunkKey& key) {
    uint64_t hash = keyToHash(key);
    
    auto it = cache.find(hash);
    if (it == cache.end()) return;
    
    currentMemoryBytes -= it->second.sizeBytes;
    
    // Remove from LRU list
    auto iterIt = keyToIterator.find(hash);
    if (iterIt != keyToIterator.end()) {
        lruOrder.erase(iterIt->second);
        keyToIterator.erase(iterIt);
    }
    
    cache.erase(it);
}

bool ChunkCache::hasChunk(const PlanetChunkKey& key) const {
    uint64_t hash = keyToHash(key);
    return cache.find(hash) != cache.end();
}

void ChunkCache::clear() {
    cache.clear();
    lruOrder.clear();
    keyToIterator.clear();
    currentMemoryBytes = 0;
}

void ChunkCache::setMaxMemory(size_t newMaxMemoryMB) {
    maxMemoryBytes = std::max<size_t>(16 * 1024 * 1024, newMaxMemoryMB * 1024 * 1024);
    evictToFitMemory();
}

uint64_t ChunkCache::keyToHash(const PlanetChunkKey& key) {
    // Create a hash from the key components
    // Using bit packing for efficiency
    uint64_t hash = 0;
    hash |= (static_cast<uint64_t>(key.face) & 0x7);  // 3 bits for face [0,5]
    hash |= ((static_cast<uint64_t>(key.lod) & 0x1F) << 3);  // 5 bits for LOD [0,31]
    hash |= ((static_cast<uint64_t>(key.x) & 0xFFF) << 8);  // 12 bits for X
    hash |= ((static_cast<uint64_t>(key.y) & 0xFFF) << 20); // 12 bits for Y
    return hash;
}

bool ChunkCache::evictLRU() {
    if (lruOrder.empty()) {
        return false;
    }
    
    // Evict the least recently used chunk (front of the list)
    PlanetChunkKey lruKey = lruOrder.front();
    lruOrder.pop_front();
    
    uint64_t hash = keyToHash(lruKey);
    auto it = cache.find(hash);
    if (it != cache.end()) {
        currentMemoryBytes -= it->second.sizeBytes;
        cache.erase(it);
        stats.evictions++;
    }
    
    keyToIterator.erase(hash);
    return true;
}

void ChunkCache::evictToFitMemory() {
    while (currentMemoryBytes > maxMemoryBytes && !lruOrder.empty()) {
        evictLRU();
    }
}

void ChunkCache::updateLRUOrder(const PlanetChunkKey& key) {
    uint64_t hash = keyToHash(key);
    
    // Find the iterator for this key
    auto iterIt = keyToIterator.find(hash);
    if (iterIt == keyToIterator.end()) {
        return;  // Key not in list
    }
    
    // Move the key to the end (most recently used)
    auto iter = iterIt->second;
    lruOrder.erase(iter);
    lruOrder.push_back(key);
    keyToIterator[hash] = std::prev(lruOrder.end());
}

} // namespace Haruka
