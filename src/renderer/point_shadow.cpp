#include "point_shadow.h"

#include <stdexcept>

PointShadow::PointShadow(unsigned int width, unsigned int height)
    : width(width), height(height) {}

PointShadow::~PointShadow() {
    // El usuario debe llamar a destroyVulkanResources antes del destructor
}

void PointShadow::createVulkanResources(VkDevice device, VkPhysicalDevice physicalDevice, VkRenderPass renderPass, uint32_t w, uint32_t h) {
    width = w;
    height = h;
    VkFormat format = VK_FORMAT_D32_SFLOAT;
    // Crear imagen para depth cubemap
    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.extent.width = w;
    imgInfo.extent.height = h;
    imgInfo.extent.depth = 1;
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 6; // Cubemap
    imgInfo.format = format;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imgInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (vkCreateImage(device, &imgInfo, nullptr, &vkShadowImage) != VK_SUCCESS)
        throw std::runtime_error("Failed to create PointShadow image");
    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, vkShadowImage, &memReq);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    uint32_t memoryTypeIndex = 0;
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((memReq.memoryTypeBits & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memoryTypeIndex = i;
            break;
        }
    }
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &vkShadowMemory) != VK_SUCCESS)
        throw std::runtime_error("Failed to allocate PointShadow image memory");
    vkBindImageMemory(device, vkShadowImage, vkShadowMemory, 0);
    // Crear view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = vkShadowImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 6;
    if (vkCreateImageView(device, &viewInfo, nullptr, &vkShadowView) != VK_SUCCESS)
        throw std::runtime_error("Failed to create PointShadow image view");
    // Crear framebuffer
    VkImageView attachments[] = { vkShadowView };
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = attachments;
    fbInfo.width = w;
    fbInfo.height = h;
    fbInfo.layers = 6;
    if (vkCreateFramebuffer(device, &fbInfo, nullptr, &vkShadowFramebuffer) != VK_SUCCESS)
        throw std::runtime_error("Failed to create PointShadow framebuffer");
}

void PointShadow::destroyVulkanResources(VkDevice device) {
    if (vkShadowFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, vkShadowFramebuffer, nullptr);
        vkShadowFramebuffer = VK_NULL_HANDLE;
    }
    if (vkShadowView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, vkShadowView, nullptr);
        vkShadowView = VK_NULL_HANDLE;
    }
    if (vkShadowImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, vkShadowImage, nullptr);
        vkShadowImage = VK_NULL_HANDLE;
    }
    if (vkShadowMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, vkShadowMemory, nullptr);
        vkShadowMemory = VK_NULL_HANDLE;
    }
}