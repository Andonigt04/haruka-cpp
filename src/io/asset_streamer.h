#pragma once

#include <string>
#include <memory>
#include <map>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <glm/glm.hpp>

/**
 * @brief On-demand asynchronous asset streaming service.
 *
 * Features:
 * - background loading workers
 * - camera-distance/importance prioritization
 * - LRU-oriented cache trimming
 * - callback notification on load completion
 */

enum class AssetType {
    TEXTURE,
    MODEL,
    SHADER,
    AUDIO,
    UNKNOWN
};

struct Asset {
    std::string id;
    std::string path;
    AssetType type;
    void* data = nullptr;
    size_t sizeBytes = 0;
    bool loaded = false;
    float priority = 0.0f;  // Higher value = higher priority
    long lastAccessTime = 0;
};

struct StreamRequest {
    std::string assetId;
    std::string assetPath;
    AssetType type;
    glm::vec3 position;
    float importance = 1.0f;  // 0.0-1.0
};

class AssetStreamer {
public:
    static AssetStreamer& getInstance() {
        static AssetStreamer instance;
        return instance;
    }

    /**
     * @brief Initializes worker threads and cache limits.
     * @param maxCacheMemoryMB Cache memory budget in MB.
     * @param numWorkerThreads Number of async loader workers.
     */
    void init(size_t maxCacheMemoryMB = 512, int numWorkerThreads = 2);

    /** @brief Enqueues an asset streaming request. */
    void requestAsset(
        const std::string& assetId,
        const std::string& assetPath,
        AssetType type,
        float importance = 1.0f,
        const glm::vec3& position = glm::vec3(0.0f)
    );

    /** @brief Blocking fetch with timeout while waiting for load completion. */
    Asset* getAsset(const std::string& assetId, float timeoutMs = 5000.0f);

    /** @brief Non-blocking cache lookup. */
    Asset* tryGetAsset(const std::string& assetId);

    /** @brief Updates camera reference used by prioritization heuristics. */
    void updateCameraPosition(const glm::vec3& position);

    /** @brief Explicitly unloads one asset from cache. */
    void unloadAsset(const std::string& assetId);

    /** @brief Trims cache according to internal eviction policy. */
    void trimCache();

    /** @brief Snapshot of streamer health and cache usage. */
    struct StreamStats {
        size_t totalCacheMemory;
        size_t maxCacheMemory;
        int loadedAssets;
        int pendingAssets;
        int failedAssets;
        float cacheUtilization;  // 0.0-1.0
    };

    StreamStats getStats() const;

    /** @brief Callback invoked when an asset is loaded and inserted in cache. */
    using AssetLoadedCallback = std::function<void(const std::string&, Asset*)>;
    void onAssetLoaded(AssetLoadedCallback callback) {
        assetLoadedCallback = callback;
    }

    /** @brief Stops workers and releases streamer resources. */
    void shutdown();

    ~AssetStreamer();

private:
    AssetStreamer();

    // Async loading worker loop
    void workerThread();

    // Synchronous loading implementation for one request
    Asset* loadAssetSync(const StreamRequest& request);

    // Estimate source file size for budgeting
    size_t estimateAssetSize(const std::string& path);

    // Reserve cache room if needed
    void makeRoomInCache(size_t neededBytes);

    // Eviction policy (LRU)
    void evictLRU();

    // Thread safety
    mutable std::mutex cacheMutex;
    mutable std::mutex queueMutex;
    std::condition_variable cv;

    // Asset cache
    std::map<std::string, std::unique_ptr<Asset>> assetCache;

    // Request queue
    std::queue<StreamRequest> loadQueue;

    // Worker threads
    std::vector<std::thread> workers;
    bool running = false;

    // Configuration
    size_t maxCacheMemoryBytes = 512 * 1024 * 1024;  // 512 MB default
    int numWorkers = 2;

    // Statistics
    size_t totalCacheMemory = 0;
    int failedAssets = 0;

    // Camera position (for prioritization)
    glm::vec3 cameraPosition = glm::vec3(0.0f);

    // Callbacks
    AssetLoadedCallback assetLoadedCallback = nullptr;
};
