#include "asset_streamer.h"
#include <iostream>
#include <chrono>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

AssetStreamer::AssetStreamer() {}

AssetStreamer::~AssetStreamer() {
    shutdown();
}

void AssetStreamer::init(size_t maxCacheMemoryMB, int numWorkerThreads) {
    maxCacheMemoryBytes = maxCacheMemoryMB * 1024 * 1024;
    numWorkers = numWorkerThreads;
    running = true;

    // Crear worker threads
    for (int i = 0; i < numWorkers; i++) {
        workers.emplace_back(&AssetStreamer::workerThread, this);
    }

    std::cout << "✓ Asset Streamer initialized\n";
    std::cout << "  Cache: " << maxCacheMemoryMB << " MB\n";
    std::cout << "  Workers: " << numWorkerThreads << " threads\n";
}

void AssetStreamer::shutdown() {
    running = false;
    cv.notify_all();

    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    assetCache.clear();
    std::cout << "✓ Asset Streamer shutdown\n";
}

void AssetStreamer::requestAsset(
    const std::string& assetId,
    const std::string& assetPath,
    AssetType type,
    float importance,
    const glm::vec3& position) {

    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        
        // Si ya está cargado, actualizar prioridad
        if (assetCache.find(assetId) != assetCache.end()) {
            assetCache[assetId]->lastAccessTime = 
                std::chrono::system_clock::now().time_since_epoch().count();
            return;
        }
    }

    // Encolar para carga asincrónica
    StreamRequest request;
    request.assetId = assetId;
    request.assetPath = assetPath;
    request.type = type;
    request.position = position;
    request.importance = importance;

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        loadQueue.push(request);
    }

    cv.notify_one();
}

Asset* AssetStreamer::getAsset(const std::string& assetId, float timeoutMs) {
    auto startTime = std::chrono::high_resolution_clock::now();

    while (true) {
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            auto it = assetCache.find(assetId);
            if (it != assetCache.end() && it->second->loaded) {
                it->second->lastAccessTime = 
                    std::chrono::system_clock::now().time_since_epoch().count();
                return it->second.get();
            }
        }

        auto elapsed = std::chrono::duration<float, std::milli>(
            std::chrono::high_resolution_clock::now() - startTime
        ).count();

        if (elapsed > timeoutMs) {
            return nullptr;  // Timeout
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

Asset* AssetStreamer::tryGetAsset(const std::string& assetId) {
    std::lock_guard<std::mutex> lock(cacheMutex);
    
    auto it = assetCache.find(assetId);
    if (it != assetCache.end() && it->second->loaded) {
        it->second->lastAccessTime = 
            std::chrono::system_clock::now().time_since_epoch().count();
        return it->second.get();
    }

    return nullptr;
}

void AssetStreamer::updateCameraPosition(const glm::vec3& position) {
    cameraPosition = position;
}

void AssetStreamer::unloadAsset(const std::string& assetId) {
    std::lock_guard<std::mutex> lock(cacheMutex);
    
    auto it = assetCache.find(assetId);
    if (it != assetCache.end()) {
        totalCacheMemory -= it->second->sizeBytes;
        assetCache.erase(it);
    }
}

size_t AssetStreamer::estimateAssetSize(const std::string& path) {
    try {
        if (fs::exists(path)) {
            return fs::file_size(path);
        }
    } catch (...) {}
    return 0;
}

void AssetStreamer::makeRoomInCache(size_t neededBytes) {
    while (totalCacheMemory + neededBytes > maxCacheMemoryBytes && 
           !assetCache.empty()) {
        evictLRU();
    }
}

void AssetStreamer::evictLRU() {
    std::string lruKey;
    long oldestTime = LLONG_MAX;

    for (auto& pair : assetCache) {
        if (pair.second->lastAccessTime < oldestTime) {
            oldestTime = pair.second->lastAccessTime;
            lruKey = pair.first;
        }
    }

    if (!lruKey.empty()) {
        totalCacheMemory -= assetCache[lruKey]->sizeBytes;
        assetCache.erase(lruKey);
        std::cout << "💾 Evicted LRU asset: " << lruKey << "\n";
    }
}

Asset* AssetStreamer::loadAssetSync(const StreamRequest& request) {
    auto asset = std::make_unique<Asset>();
    asset->id = request.assetId;
    asset->path = request.assetPath;
    asset->type = request.type;
    asset->sizeBytes = estimateAssetSize(request.assetPath);

    // Simular carga (en producción, parsear archivo real)
    try {
        if (fs::exists(request.assetPath)) {
            asset->data = malloc(asset->sizeBytes);
            asset->loaded = true;

            std::cout << "✓ Loaded asset: " << request.assetId 
                      << " (" << asset->sizeBytes / 1024 << " KB)\n";
        } else {
            std::cerr << "✗ Asset not found: " << request.assetPath << "\n";
            failedAssets++;
            return nullptr;
        }
    } catch (const std::exception& e) {
        std::cerr << "✗ Failed to load asset: " << e.what() << "\n";
        failedAssets++;
        return nullptr;
    }

    return asset.get();
}

void AssetStreamer::workerThread() {
    while (running) {
        StreamRequest request;

        {
            std::unique_lock<std::mutex> lock(queueMutex);
            cv.wait(lock, [this] { return !loadQueue.empty() || !running; });

            if (!running) break;
            if (loadQueue.empty()) continue;

            request = loadQueue.front();
            loadQueue.pop();
        }

        // Cargar asset
        Asset* loadedAsset = loadAssetSync(request);

        if (loadedAsset) {
            std::lock_guard<std::mutex> lock(cacheMutex);
            
            // Verificar espacio en cache
            makeRoomInCache(loadedAsset->sizeBytes);

            // Agregar al cache
            auto asset = std::make_unique<Asset>(*loadedAsset);
            totalCacheMemory += asset->sizeBytes;
            
            Asset* ptr = asset.get();
            assetCache[request.assetId] = std::move(asset);

            // Callback
            if (assetLoadedCallback) {
                assetLoadedCallback(request.assetId, ptr);
            }
        }
    }
}

void AssetStreamer::trimCache() {
    std::lock_guard<std::mutex> lock(cacheMutex);

    // Remover assets no usados recientemente
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    const long TIMEOUT_NS = 30 * 1000000000LL;  // 30 segundos

    std::vector<std::string> toRemove;
    for (auto& pair : assetCache) {
        if (now - pair.second->lastAccessTime > TIMEOUT_NS) {
            toRemove.push_back(pair.first);
        }
    }

    for (const auto& key : toRemove) {
        totalCacheMemory -= assetCache[key]->sizeBytes;
        assetCache.erase(key);
    }

    std::cout << "💾 Cache trimmed: " << toRemove.size() << " assets removed\n";
}

AssetStreamer::StreamStats AssetStreamer::getStats() const {
    std::lock_guard<std::mutex> lock(cacheMutex);

    StreamStats stats;
    stats.totalCacheMemory = totalCacheMemory;
    stats.maxCacheMemory = maxCacheMemoryBytes;
    stats.loadedAssets = assetCache.size();
    
    {
        std::lock_guard<std::mutex> qLock(queueMutex);
        stats.pendingAssets = loadQueue.size();
    }
    
    stats.failedAssets = failedAssets;
    stats.cacheUtilization = static_cast<float>(totalCacheMemory) / maxCacheMemoryBytes;

    return stats;
}
