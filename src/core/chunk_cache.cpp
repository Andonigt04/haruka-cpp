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
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t hash = keyToHash(key);
    auto it = cache.find(hash);

    if (it != cache.end()) {
        stats.hits++;
        updateLRUOrder(key);
        return &it->second.data;
    }

    stats.misses++;
    return nullptr;
}

bool ChunkCache::getChunkCopy(const PlanetChunkKey& key, ChunkData& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = cache.find(keyToHash(key));
    if (it == cache.end()) { stats.misses++; return false; }
    stats.hits++;
    updateLRUOrder(key);
    out = it->second.data;   // COPY under the lock → safe from concurrent rehash/evict
    return true;
}

void ChunkCache::addChunk(const PlanetChunkKey& key, const ChunkData& data) {
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t hash = keyToHash(key);

    size_t chunkSizeBytes = 0;
    chunkSizeBytes += data.vertices.size() * sizeof(glm::vec3);
    chunkSizeBytes += data.normals.size() * sizeof(glm::vec3);
    chunkSizeBytes += data.colors.size() * sizeof(glm::vec3);
    chunkSizeBytes += data.indices.size() * sizeof(unsigned int);

    auto existing = cache.find(hash);
    if (existing != cache.end()) {
        currentMemoryBytes -= existing->second.sizeBytes;
        existing->second.data = data;
        existing->second.sizeBytes = chunkSizeBytes;
        currentMemoryBytes += chunkSizeBytes;
        updateLRUOrder(key);
    } else {
        CacheEntry entry{data, chunkSizeBytes};
        cache[hash] = entry;
        lruOrder.push_back(key);
        keyToIterator[hash] = std::prev(lruOrder.end());
        currentMemoryBytes += chunkSizeBytes;
    }

    evictToFitMemory();
}

void ChunkCache::removeChunk(const PlanetChunkKey& key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t hash = keyToHash(key);

    auto it = cache.find(hash);
    if (it == cache.end()) return;

    currentMemoryBytes -= it->second.sizeBytes;

    auto iterIt = keyToIterator.find(hash);
    if (iterIt != keyToIterator.end()) {
        lruOrder.erase(iterIt->second);
        keyToIterator.erase(iterIt);
    }

    cache.erase(it);
}

bool ChunkCache::hasChunk(const PlanetChunkKey& key) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t hash = keyToHash(key);
    return cache.find(hash) != cache.end();
}

void ChunkCache::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    cache.clear();
    lruOrder.clear();
    keyToIterator.clear();
    currentMemoryBytes = 0;
}

void ChunkCache::setMaxMemory(size_t newMaxMemoryMB) {
    std::lock_guard<std::mutex> lock(m_mutex);
    maxMemoryBytes = std::max<size_t>(16 * 1024 * 1024, newMaxMemoryMB * 1024 * 1024);
    evictToFitMemory();
}

uint64_t ChunkCache::keyToHash(const PlanetChunkKey& key) {
    uint64_t hash = 0;
    hash |= (static_cast<uint64_t>(key.face) & 0x7);
    hash |= ((static_cast<uint64_t>(key.lod) & 0x1F) << 3);
    hash |= ((static_cast<uint64_t>(key.x) & 0xFFF) << 8);
    hash |= ((static_cast<uint64_t>(key.y) & 0xFFF) << 20);
    return hash;
}

bool ChunkCache::evictLRU() {
    if (lruOrder.empty()) return false;

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

    auto iterIt = keyToIterator.find(hash);
    if (iterIt == keyToIterator.end()) return;

    auto iter = iterIt->second;
    lruOrder.erase(iter);
    lruOrder.push_back(key);
    keyToIterator[hash] = std::prev(lruOrder.end());
}

} // namespace Haruka
