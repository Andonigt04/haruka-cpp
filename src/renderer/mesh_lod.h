#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <vulkan/vulkan.h>
#include "mesh_optimizer.h"

/**
 * @brief Mesh level-of-detail manager.
 *
 * Generates and renders simplified mesh variants based on camera distance.
 */
class MeshLOD {
public:
    MeshLOD();
    ~MeshLOD();

    void generateLODs(
        const std::vector<Vertex>& vertices,
        const std::vector<unsigned int>& indices,
        const std::vector<float>& distances = {10.0f, 30.0f, 100.0f}
    );

    int selectLOD(float distance) const;
    void render(VkCommandBuffer cmd, VkPipeline pipeline, VkPipelineLayout layout, float distance);

    struct LODStats {
        int totalLODLevels;
        int totalVertices;
        int totalIndices;
        float memoryReduction;
    };
    LODStats getStats() const;

private:
    struct LODLevel {
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory indexMemory = VK_NULL_HANDLE;
        uint32_t indexCount = 0;
        std::vector<Vertex> vertices;
        std::vector<unsigned int> indices;
        float minDistance = 0.0f, maxDistance = 0.0f;
    };
    std::vector<LODLevel> lodLevels;
    MeshOptimizer optimizer;
    void setupVulkan(LODLevel& level, VkDevice device, VkPhysicalDevice physicalDevice, VkQueue transferQueue, VkCommandPool commandPool);
    void destroyVulkan(LODLevel& level, VkDevice device);
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkQueue transferQueue = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
};

