#include "mesh_lod.h"
#include <algorithm>
#include <cstring>

MeshLOD::MeshLOD() {}

MeshLOD::~MeshLOD() {
    for (auto& lod : lodLevels) {
        if (device != VK_NULL_HANDLE) destroyVulkan(lod, device);
    }
}

void MeshLOD::setupVulkan(LODLevel& level, VkDevice device, VkPhysicalDevice physicalDevice, VkQueue transferQueue, VkCommandPool commandPool) {
    // Crear vertex buffer
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = level.vertices.size() * sizeof(Vertex);
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device, &bufferInfo, nullptr, &level.vertexBuffer);
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, level.vertexBuffer, &memReq);
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
    vkAllocateMemory(device, &allocInfo, nullptr, &level.vertexMemory);
    vkBindBufferMemory(device, level.vertexBuffer, level.vertexMemory, 0);
    void* data;
    vkMapMemory(device, level.vertexMemory, 0, bufferInfo.size, 0, &data);
    memcpy(data, level.vertices.data(), bufferInfo.size);
    vkUnmapMemory(device, level.vertexMemory);
    // Crear index buffer
    bufferInfo.size = level.indices.size() * sizeof(unsigned int);
    bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    vkCreateBuffer(device, &bufferInfo, nullptr, &level.indexBuffer);
    vkGetBufferMemoryRequirements(device, level.indexBuffer, &memReq);
    allocInfo.allocationSize = memReq.size;
    vkAllocateMemory(device, &allocInfo, nullptr, &level.indexMemory);
    vkBindBufferMemory(device, level.indexBuffer, level.indexMemory, 0);
    vkMapMemory(device, level.indexMemory, 0, bufferInfo.size, 0, &data);
    memcpy(data, level.indices.data(), bufferInfo.size);
    vkUnmapMemory(device, level.indexMemory);
    level.indexCount = static_cast<uint32_t>(level.indices.size());
}

void MeshLOD::destroyVulkan(LODLevel& level, VkDevice device) {
    if (level.vertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, level.vertexBuffer, nullptr);
        level.vertexBuffer = VK_NULL_HANDLE;
    }
    if (level.vertexMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, level.vertexMemory, nullptr);
        level.vertexMemory = VK_NULL_HANDLE;
    }
    if (level.indexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, level.indexBuffer, nullptr);
        level.indexBuffer = VK_NULL_HANDLE;
    }
    if (level.indexMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, level.indexMemory, nullptr);
        level.indexMemory = VK_NULL_HANDLE;
    }
}

void MeshLOD::generateLODs(
    const std::vector<Vertex>& vertices,
    const std::vector<unsigned int>& indices,
    const std::vector<float>& distances) {
    lodLevels.clear();
    // LOD 0: Original mesh
    {
        LODLevel lod0;
        lod0.vertices = vertices;
        lod0.indices = indices;
        lod0.minDistance = 0.0f;
        lod0.maxDistance = distances.size() > 0 ? distances[0] : 10.0f;
        if (device != VK_NULL_HANDLE)
            setupVulkan(lod0, device, physicalDevice, transferQueue, commandPool);
        lodLevels.push_back(lod0);
    }
    for (size_t i = 1; i < 4; i++) {
        if (i - 1 >= distances.size()) break;
        auto optimized = optimizer.generateLOD(vertices, indices, i);
        LODLevel lod;
        lod.vertices = optimized.vertices;
        lod.indices = optimized.indices;
        lod.minDistance = distances[i - 1];
        lod.maxDistance = (i < distances.size()) ? distances[i] : 1000.0f;
        if (device != VK_NULL_HANDLE)
            setupVulkan(lod, device, physicalDevice, transferQueue, commandPool);
        lodLevels.push_back(lod);
    }
}

int MeshLOD::selectLOD(float distance) const {
    for (size_t i = 0; i < lodLevels.size(); i++) {
        if (distance >= lodLevels[i].minDistance && 
            distance < lodLevels[i].maxDistance) {
            return i;
        }
    }
    return lodLevels.size() - 1;
}

void MeshLOD::render(VkCommandBuffer cmd, VkPipeline pipeline, VkPipelineLayout layout, float distance) {
    if (lodLevels.empty()) return;
    int lodIdx = selectLOD(distance);
    const auto& lod = lodLevels[lodIdx];
    VkDeviceSize offsets[] = {0};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindVertexBuffers(cmd, 0, 1, &lod.vertexBuffer, offsets);
    vkCmdBindIndexBuffer(cmd, lod.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    // TODO: Bindear descriptor sets si es necesario
    vkCmdDrawIndexed(cmd, lod.indexCount, 1, 0, 0, 0);
}

MeshLOD::LODStats MeshLOD::getStats() const {
    LODStats stats{};
    stats.totalLODLevels = lodLevels.size();
    for (const auto& lod : lodLevels) {
        stats.totalVertices += lod.vertices.size();
        stats.totalIndices += lod.indices.size();
    }
    if (lodLevels.size() > 0) {
        int originalSize = lodLevels[0].vertices.size() * lodLevels[0].indices.size();
        int optimizedSize = stats.totalVertices * stats.totalIndices;
        if (originalSize > 0) {
            stats.memoryReduction = 1.0f - (static_cast<float>(optimizedSize) / originalSize);
        }
    }
    return stats;
}
