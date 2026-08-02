#pragma once

#include <glm/glm.hpp>
#include "rhi/rhi_types.h"
#include <memory>
#include <vector>
#include <map>
#include <queue>
#include <unordered_map>
#include <string>

namespace std {
    template<> struct hash<glm::uvec2> {
        size_t operator()(const glm::uvec2& v) const {
            return hash<unsigned int>()(v.x) ^ (hash<unsigned int>()(v.y) << 1);
        }
    };
}

namespace Haruka { namespace Renderer {

struct PageRequest {
    glm::uvec2 page;
    int mipLevel;
    float priority;
};

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
    VirtualTexturing();
    ~VirtualTexturing();

    void init(const VTConfig& config = VTConfig());
    void addVirtualTexture(const std::string& textureId, const std::string& filePath, glm::uvec2 virtualSize);
    void processFeedback();

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
        std::string id, filePath;
        glm::uvec2 virtualSize, pageTableSize;
        Haruka::RHI::TextureHandle hIndirection, hPhysical, hFeedback;
        std::queue<PageRequest> pageQueue;
        std::unordered_map<glm::uvec2, bool> residentPages;
        int residentPageCount = 0;
        size_t memoryUsage = 0;
    };

    VTConfig config;
    std::map<std::string, std::unique_ptr<VirtualTextureData>> virtualTextures;
    size_t totalMemoryUsage = 0;
    std::queue<PageRequest> globalPageQueue;
};

}} // namespace Haruka::Renderer

using Haruka::Renderer::PageRequest;
using Haruka::Renderer::VTConfig;
using Haruka::Renderer::VirtualTexturing;
namespace Haruka { using Renderer::PageRequest; using Renderer::VTConfig; using Renderer::VirtualTexturing; }