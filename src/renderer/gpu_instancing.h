#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <glm/glm.hpp>

class GPUInstancing {
public:
    GPUInstancing();
    ~GPUInstancing();

    void init(int maxInst, VkDevice device, VkPhysicalDevice physicalDevice, VkQueue transferQueue, VkCommandPool commandPool);
    void updateBuffer(VkDevice device);
    void render(VkCommandBuffer cmd, VkPipeline pipeline, VkPipelineLayout layout, VkBuffer meshVertexBuffer, VkBuffer meshIndexBuffer, uint32_t indexCount);
    void clear();

    enum PrecisionMode { PRECISION_DOUBLE, PRECISION_FLOAT };
    PrecisionMode precisionMode = PRECISION_FLOAT;

    struct InstanceDataDouble {
        glm::dmat4 model;
        glm::dvec3 position;
        glm::vec4 color;
        glm::vec3 scale;
    };
    struct InstanceDataFloat {
        glm::mat4 model;
        glm::vec4 color;
        glm::vec3 scale;
    };

    std::vector<InstanceDataDouble> instancesDouble;
    std::vector<InstanceDataFloat> instancesFloat;
    int maxInstances = 0;
    bool bufferDirty = false;

private:
    VkBuffer instanceBuffer = VK_NULL_HANDLE;
    VkDeviceMemory instanceMemory = VK_NULL_HANDLE;
    size_t instanceCount = 0;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkQueue transferQueue = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    void setupInstanceBuffer();
    void destroyInstanceBuffer();
};
