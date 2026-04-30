#include "asset_streamer.h"
#include <iostream>
#include <fstream>
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

    try {
        running = true;
        for (int i = 0; i < numWorkers; i++)
            workers.emplace_back(&AssetStreamer::workerThread, this);
        healthy = true;
        std::cout << "✓ Asset Streamer initialized ("
                  << maxCacheMemoryMB << " MB, "
                  << numWorkerThreads << " workers)\n";
    } catch (const std::exception& e) {
        running = false;
        healthy = false;
        HARUKA_ASSET_ERROR(ErrorCode::ASSET_STREAMER_INIT_FAILED,
            std::string("init failed, falling back to sync loading: ") + e.what());
    }
}

void AssetStreamer::shutdown() {
    running = false;
    cv.notify_all();

    for (auto& worker : workers) {
        if (worker.joinable()) {
            try {
                worker.join();
            } catch (const std::system_error& e) {
                HARUKA_IO_ERROR(ErrorCode::THREAD_ERROR,
                    std::string("thread join failed during shutdown: ") + e.what());
            }
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
        if (assetCache.count(assetId)) {
            assetCache[assetId]->lastAccessTime =
                std::chrono::system_clock::now().time_since_epoch().count();
            return;
        }
    }

    StreamRequest request{assetId, assetPath, type, position, importance};

    // If async workers are not available, load synchronously as fallback.
    if (!healthy) {
        HARUKA_ASSET_ERROR(ErrorCode::ASSET_STREAMER_INIT_FAILED,
            "async system unavailable, loading '" + assetId + "' synchronously");
        auto asset = loadAssetSync(request);
        if (asset) {
            std::lock_guard<std::mutex> lock(cacheMutex);
            makeRoomInCache(asset->sizeBytes);
            totalCacheMemory += asset->sizeBytes;
            assetCache[assetId] = std::move(asset);
        } else {
            HARUKA_ASSET_ERROR(ErrorCode::ASSET_NOT_FOUND,
                "sync fallback also failed for '" + assetId + "'");
        }
        return;
    }

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
            HARUKA_ASSET_ERROR(ErrorCode::ASSET_LOAD_TIMEOUT,
                "getAsset timeout for '" + assetId + "' after "
                + std::to_string(static_cast<int>(timeoutMs)) + " ms");
            return nullptr;
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
        if (fs::exists(path))
            return fs::file_size(path);
    } catch (const std::exception& e) {
        HARUKA_IO_ERROR(ErrorCode::FILE_READ_ERROR,
            std::string("estimateAssetSize failed for '") + path + "': " + e.what());
    }
    return 0;
}

void AssetStreamer::makeRoomInCache(size_t neededBytes) {
    while (totalCacheMemory + neededBytes > maxCacheMemoryBytes &&
           !assetCache.empty()) {
        evictLRU();
    }
    if (totalCacheMemory + neededBytes > maxCacheMemoryBytes) {
        HARUKA_ASSET_ERROR(ErrorCode::ASSET_CACHE_FULL,
            "cache full — could not evict enough assets to fit "
            + std::to_string(neededBytes / 1024) + " KB");
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

std::unique_ptr<Asset> AssetStreamer::loadAssetSync(const StreamRequest& request) {
    auto asset = std::make_unique<Asset>();
    asset->id = request.assetId;
    asset->path = request.assetPath;
    asset->type = request.type;
    asset->sizeBytes = estimateAssetSize(request.assetPath);

    try {
        if (!fs::exists(request.assetPath)) {
            HARUKA_ASSET_ERROR(ErrorCode::ASSET_NOT_FOUND,
                "asset not found: '" + request.assetPath + "'");
            failedAssets++;
            return nullptr;
        }

        std::ifstream file(request.assetPath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            HARUKA_IO_ERROR(ErrorCode::FILE_READ_ERROR,
                "cannot open asset: '" + request.assetPath + "'");
            failedAssets++;
            return nullptr;
        }

        asset->sizeBytes = static_cast<size_t>(file.tellg());
        file.seekg(0, std::ios::beg);
        asset->data = malloc(asset->sizeBytes);
        if (!asset->data) {
            HARUKA_IO_ERROR(ErrorCode::OUT_OF_MEMORY,
                "out of memory loading '" + request.assetPath
                + "' (" + std::to_string(asset->sizeBytes / 1024) + " KB)");
            failedAssets++;
            return nullptr;
        }
        file.read(static_cast<char*>(asset->data), asset->sizeBytes);
        asset->loaded = true;
        std::cout << "✓ Loaded asset: " << request.assetId
                  << " (" << asset->sizeBytes / 1024 << " KB)\n";
        return asset;
    } catch (const std::exception& e) {
        HARUKA_ASSET_ERROR(ErrorCode::ASSET_CORRUPTED,
            std::string("exception loading '") + request.assetPath + "': " + e.what());
        failedAssets++;
        return nullptr;
    }
}

void AssetStreamer::workerThread() {
    try {
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

            auto loadedAsset = loadAssetSync(request);
            if (loadedAsset) {
                std::lock_guard<std::mutex> lock(cacheMutex);
                makeRoomInCache(loadedAsset->sizeBytes);
                totalCacheMemory += loadedAsset->sizeBytes;
                Asset* ptr = loadedAsset.get();
                assetCache[request.assetId] = std::move(loadedAsset);
                if (assetLoadedCallback)
                    assetLoadedCallback(request.assetId, ptr);
            }
        }
    } catch (const std::exception& e) {
        healthy = false;
        HARUKA_ASSET_ERROR(ErrorCode::ASSET_STREAMER_INIT_FAILED,
            std::string("worker thread crashed: ") + e.what()
            + " — future requests will use sync fallback");
    } catch (...) {
        healthy = false;
        HARUKA_ASSET_ERROR(ErrorCode::ASSET_STREAMER_INIT_FAILED,
            "worker thread crashed (unknown exception) — future requests will use sync fallback");
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
