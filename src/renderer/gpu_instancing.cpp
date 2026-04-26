#include "gpu_instancing.h"

#include <cstring>
#include <glm/gtc/matrix_transform.hpp>

GPUInstancing::GPUInstancing() {}

GPUInstancing::~GPUInstancing() {
    destroyInstanceBuffer();
}

void GPUInstancing::init(int maxInst, VkDevice device_, VkPhysicalDevice physicalDevice_, VkQueue transferQueue_, VkCommandPool commandPool_) {
    maxInstances = maxInst;
    device = device_;
    physicalDevice = physicalDevice_;
    transferQueue = transferQueue_;
    commandPool = commandPool_;
    instancesDouble.reserve(maxInstances);
    instancesFloat.reserve(maxInstances);
    setupInstanceBuffer();
}

void GPUInstancing::setupInstanceBuffer() {
    if (instanceBuffer != VK_NULL_HANDLE) destroyInstanceBuffer();
    size_t elementSize = (precisionMode == PRECISION_DOUBLE) ? sizeof(InstanceDataDouble) : sizeof(InstanceDataFloat);
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = maxInstances * elementSize;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device, &bufferInfo, nullptr, &instanceBuffer);
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, instanceBuffer, &memReq);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    uint32_t memoryTypeIndex = 0;
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((memReq.memoryTypeBits & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memoryTypeIndex = i;
            break;
        }
    }
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    vkAllocateMemory(device, &allocInfo, nullptr, &instanceMemory);
    vkBindBufferMemory(device, instanceBuffer, instanceMemory, 0);
}

void GPUInstancing::destroyInstanceBuffer() {
    if (instanceBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, instanceBuffer, nullptr);
        instanceBuffer = VK_NULL_HANDLE;
    }
    if (instanceMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, instanceMemory, nullptr);
        instanceMemory = VK_NULL_HANDLE;
    }
}

void GPUInstancing::updateBuffer(VkDevice device) {
    if (!bufferDirty) return;
    size_t elementSize = (precisionMode == PRECISION_DOUBLE) ? sizeof(InstanceDataDouble) : sizeof(InstanceDataFloat);
    void* data;
    vkMapMemory(device, instanceMemory, 0, maxInstances * elementSize, 0, &data);
    if (precisionMode == PRECISION_DOUBLE && !instancesDouble.empty()) {
        memcpy(data, instancesDouble.data(), instancesDouble.size() * sizeof(InstanceDataDouble));
        instanceCount = instancesDouble.size();
    } else if (precisionMode == PRECISION_FLOAT && !instancesFloat.empty()) {
        memcpy(data, instancesFloat.data(), instancesFloat.size() * sizeof(InstanceDataFloat));
        instanceCount = instancesFloat.size();
    }
    vkUnmapMemory(device, instanceMemory);
    bufferDirty = false;
}

void GPUInstancing::render(VkCommandBuffer cmd, VkPipeline pipeline, VkPipelineLayout layout, VkBuffer meshVertexBuffer, VkBuffer meshIndexBuffer, uint32_t indexCount) {
    if (instanceCount == 0) return;
    VkDeviceSize offsets[] = {0};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkBuffer vertexBuffers[] = {meshVertexBuffer, instanceBuffer};
    VkDeviceSize bufferOffsets[] = {0, 0};
    vkCmdBindVertexBuffers(cmd, 0, 2, vertexBuffers, bufferOffsets);
    vkCmdBindIndexBuffer(cmd, meshIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
    // TODO: Bindear descriptor sets si es necesario
    vkCmdDrawIndexed(cmd, indexCount, static_cast<uint32_t>(instanceCount), 0, 0, 0);
}

void GPUInstancing::clear() {
    instancesDouble.clear();
    instancesFloat.clear();
    bufferDirty = true;
}
