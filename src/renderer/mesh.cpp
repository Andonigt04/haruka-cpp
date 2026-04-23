#include "mesh.h"

#include <cstring>

#include "../core/application.h" // Para acceso a vkMaterialDescriptorSetLayout y vkDescriptorPool

// Constructor para modelos complejos (con texturas)
Mesh::Mesh(std::vector<Vertex> vertex, std::vector<unsigned int> idx, std::vector<MeshTexture> textures) {
    this->vertex = vertex;
    this->index = idx;
    this->textures = textures;
    // Vulkan: inicialización de buffers y recursos debe llamarse explícitamente
}

// Constructor simplificado (solo geometria)
Mesh::Mesh(const std::vector<glm::vec3>& vertices,
           const std::vector<glm::vec3>& normals,
           const std::vector<unsigned int>& indices) {
    this->index = indices;
    this->vertex.clear();
    this->textures.clear();
    // Vulkan: inicialización de buffers y recursos debe llamarse explícitamente
}

Mesh::~Mesh() {
    // Vulkan: liberar buffers y recursos debe llamarse explícitamente
}

void Mesh::createVulkanBuffers(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue transferQueue, VkCommandPool commandPool) {
    if (!vertex.empty()) {
        vertexBuffer.create(device, physicalDevice, vertex.data(), vertex.size() * sizeof(Vertex), transferQueue, commandPool);
    }
    if (!index.empty()) {
        indexBuffer.create(device, physicalDevice, index.data(), index.size(), transferQueue, commandPool);
    }
}

void Mesh::destroyVulkanBuffers() {
    vertexBuffer.destroy();
    indexBuffer.destroy();
}

void Mesh::draw(VkCommandBuffer cmd) const {
    if (vertexBuffer.buffer == VK_NULL_HANDLE || indexBuffer.buffer == VK_NULL_HANDLE) return;
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer.buffer, offsets);
    vkCmdBindIndexBuffer(cmd, indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, static_cast<uint32_t>(index.size()), 1, 0, 0, 0);
}

void MeshTexture::createVulkanImage(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t width, uint32_t height, const void* pixels) {
    // Simplificado: solo imagen 2D, sin staging buffer, uso HOST_VISIBLE para ejemplo
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    imageInfo.tiling = VK_IMAGE_TILING_LINEAR;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateImage(device, &imageInfo, nullptr, &vkImage);

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device, vkImage, &memRequirements);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    uint32_t memoryTypeIndex = 0;
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((memRequirements.memoryTypeBits & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memoryTypeIndex = i;
            break;
        }
    }
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    vkAllocateMemory(device, &allocInfo, nullptr, &vkImageMemory);
    vkBindImageMemory(device, vkImage, vkImageMemory, 0);
    // Copiar datos
    void* data;
    vkMapMemory(device, vkImageMemory, 0, memRequirements.size, 0, &data);
    memcpy(data, pixels, width * height * 4); // Asume RGBA8
    vkUnmapMemory(device, vkImageMemory);
    // Crear view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = vkImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    vkCreateImageView(device, &viewInfo, nullptr, &vkImageView);
    // Crear sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    vkCreateSampler(device, &samplerInfo, nullptr, &vkSampler);
}

void MeshTexture::destroyVulkanImage(VkDevice device) {
    if (vkSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, vkSampler, nullptr);
        vkSampler = VK_NULL_HANDLE;
    }
    if (vkImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, vkImageView, nullptr);
        vkImageView = VK_NULL_HANDLE;
    }
    if (vkImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, vkImage, nullptr);
        vkImage = VK_NULL_HANDLE;
    }
    if (vkImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, vkImageMemory, nullptr);
        vkImageMemory = VK_NULL_HANDLE;
    }
}