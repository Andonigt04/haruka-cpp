#include "bloom.h"
#include <stdexcept>

Bloom::Bloom(unsigned int width, unsigned int height)
    : width(width), height(height) {}

Bloom::~Bloom() {
    // El usuario debe llamar a destroyVulkanResources antes del destructor
}

static uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("No suitable memory type found");
}

void Bloom::createVulkanResources(VkDevice device, VkPhysicalDevice physicalDevice, VkRenderPass renderPass, uint32_t w, uint32_t h) {
    width = w;
    height = h;
    VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT; // HDR para bloom
    // Crear imagen
    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.extent.width = w;
    imgInfo.extent.height = h;
    imgInfo.extent.depth = 1;
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.format = format;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device, &imgInfo, nullptr, &vkImage) != VK_SUCCESS)
        throw std::runtime_error("Failed to create Bloom image");
    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, vkImage, &memReq);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &allocInfo, nullptr, &vkImageMemory) != VK_SUCCESS)
        throw std::runtime_error("Failed to allocate Bloom image memory");
    vkBindImageMemory(device, vkImage, vkImageMemory, 0);
    // Crear view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = vkImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &viewInfo, nullptr, &vkImageView) != VK_SUCCESS)
        throw std::runtime_error("Failed to create Bloom image view");
    // Crear framebuffer
    VkImageView attachments[] = {vkImageView};
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = attachments;
    fbInfo.width = w;
    fbInfo.height = h;
    fbInfo.layers = 1;
    if (vkCreateFramebuffer(device, &fbInfo, nullptr, &vkFramebuffer) != VK_SUCCESS)
        throw std::runtime_error("Failed to create Bloom framebuffer");
}

void Bloom::destroyVulkanResources(VkDevice device) {
    if (vkFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, vkFramebuffer, nullptr);
        vkFramebuffer = VK_NULL_HANDLE;
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