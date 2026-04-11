#pragma once

#include <glm/glm.hpp>
#include <glad/glad.h>
#include <memory>
#include <vector>
#include <map>
#include <queue>
#include <unordered_map>
#include <string>

// Custom hash para glm::uvec2
namespace std {
    template<>
    struct hash<glm::uvec2> {
        size_t operator()(const glm::uvec2& v) const {
            return hash<unsigned int>()(v.x) ^ (hash<unsigned int>()(v.y) << 1);
        }
    };
}

/**
 * @brief Page-based virtual texturing system.
 *
 * Features:
 * - page-based virtual memory
 * - feedback buffer for access tracking
 * - sparse physical residency
 * - indirection texture for UV remapping
 */

struct PageRequest {
    glm::uvec2 page;      // Page coordinate
    int mipLevel;         // Mip level
    float priority;       // Load priority
};

/** @brief Virtual texturing configuration parameters. */
struct VTConfig {
    int pageSize = 128;
    int maxPageTableSize = 8192;
    int feedbackBufferSize = 512;
    int maxMipLevels = 8;
    int maxResidentPages = 1024;
    long long maxCacheMemory = 256LL * 1024 * 1024;
};

class VirtualTexturing {
public:
    /** @brief Constructs an uninitialized virtual texturing system. */
    VirtualTexturing();
    ~VirtualTexturing();

    /** @brief Initializes the system and allocates core GPU resources. */
    void init(const VTConfig& config = VTConfig());

    /**
     * @brief Registers a virtual texture asset.
     * @param textureId Unique identifier.
     * @param filePath Source file path.
     * @param virtualSize Virtual resolution in pixels.
     */
    void addVirtualTexture(
        const std::string& textureId,
        const std::string& filePath,
        glm::uvec2 virtualSize
    );

    /** @brief Processes feedback data and updates page residency. */
    void processFeedback();

    /** @brief Binds a virtual texture for shader sampling. */
    void bindVirtualTexture(GLuint shaderProgram, const std::string& textureId);

    /** @brief Returns the indirection texture id for the requested VT. */
    GLuint getIndirectionTexture(const std::string& textureId) const;

    /** @brief Returns the physical residency texture id for the requested VT. */
    GLuint getPhysicalTexture(const std::string& textureId) const;

    /** @brief Virtual texturing runtime stats. */
    struct VTStats {
        int totalVirtualPages;
        int residentPages;
        int pageQueueSize;
        float cacheUtilization;
        int feedbackPixels;
        size_t totalMemory;
    };

    VTStats getStats(const std::string& textureId) const;

private:
    struct VirtualTextureData {
        std::string id;
        std::string filePath;
        glm::uvec2 virtualSize;
        glm::uvec2 pageTableSize;
        
        GLuint indirectionTexture = 0;  // UV → page map
        GLuint physicalTexture = 0;     // Loaded physical texture
        GLuint feedbackTexture = 0;     // Feedback buffer
        
        std::vector<GLuint> pageData;   // Page data
        std::queue<PageRequest> pageQueue;
        std::unordered_map<glm::uvec2, bool> residentPages;
        
        int residentPageCount = 0;
        size_t memoryUsage = 0;
    };

    VTConfig config;
    std::map<std::string, std::unique_ptr<VirtualTextureData>> virtualTextures;
    
    GLuint feedbackShader = 0;
    GLuint pageUploadShader = 0;
    
    size_t totalMemoryUsage = 0;
    std::queue<PageRequest> globalPageQueue;

    /** @brief Creates GPU-side resources for a virtual texture. */
    void createVirtualTextureGPUResources(VirtualTextureData& vt);
    /** @brief Loads one physical page from source media. */
    void loadPhysicalPage(VirtualTextureData& vt, const PageRequest& request);
    /** @brief Uploads a page into the physical texture atlas. */
    void uploadPageToPhysical(VirtualTextureData& vt, const PageRequest& request);
    /** @brief Refreshes the indirection texture after residency updates. */
    void updateIndirectionTexture(VirtualTextureData& vt);
    /** @brief Ensures memory budget remains within configured limits. */
    void makeRoomInCache(size_t neededBytes);
};
