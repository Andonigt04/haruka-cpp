#include "virtual_texturing.h"

#include <stdexcept>

VirtualTexturing::VirtualTexturing(unsigned int width, unsigned int height)
    : width(width), height(height) {}

VirtualTexturing::~VirtualTexturing() {
    // El usuario debe llamar a destroyVulkanResources antes del destructor
}

void VirtualTexturing::createVulkanResources(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t numPages) {
    // Ejemplo: crear numPages imágenes virtuales
    vkVirtualImages.resize(numPages);
    vkVirtualMemories.resize(numPages);
    vkVirtualViews.resize(numPages);
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    for (uint32_t i = 0; i < numPages; ++i) {
        VkImageCreateInfo imgInfo{};
        imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.extent.width = width;
        imgInfo.extent.height = height;
        imgInfo.extent.depth = 1;
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.format = format;
        imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device, &imgInfo, nullptr, &vkVirtualImages[i]) != VK_SUCCESS)
            throw std::runtime_error("Failed to create virtual texture image");
        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(device, vkVirtualImages[i], &memReq);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
        uint32_t memoryTypeIndex = 0;
        for (uint32_t j = 0; j < memProperties.memoryTypeCount; j++) {
            if ((memReq.memoryTypeBits & (1 << j)) &&
                (memProperties.memoryTypes[j].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                memoryTypeIndex = j;
                break;
            }
        }
        allocInfo.memoryTypeIndex = memoryTypeIndex;
        if (vkAllocateMemory(device, &allocInfo, nullptr, &vkVirtualMemories[i]) != VK_SUCCESS)
            throw std::runtime_error("Failed to allocate virtual texture memory");
        vkBindImageMemory(device, vkVirtualImages[i], vkVirtualMemories[i], 0);
        // Crear view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = vkVirtualImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &viewInfo, nullptr, &vkVirtualViews[i]) != VK_SUCCESS)
            throw std::runtime_error("Failed to create virtual texture view");
    }
}

void VirtualTexturing::destroyVulkanResources(VkDevice device) {
    for (size_t i = 0; i < vkVirtualViews.size(); ++i) {
        if (vkVirtualViews[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(device, vkVirtualViews[i], nullptr);
            vkVirtualViews[i] = VK_NULL_HANDLE;
        }
        if (vkVirtualImages[i] != VK_NULL_HANDLE) {
            vkDestroyImage(device, vkVirtualImages[i], nullptr);
            vkVirtualImages[i] = VK_NULL_HANDLE;
        }
        if (vkVirtualMemories[i] != VK_NULL_HANDLE) {
            vkFreeMemory(device, vkVirtualMemories[i], nullptr);
            vkVirtualMemories[i] = VK_NULL_HANDLE;
        }
    }
    vkVirtualViews.clear();
    vkVirtualImages.clear();
    vkVirtualMemories.clear();
}
