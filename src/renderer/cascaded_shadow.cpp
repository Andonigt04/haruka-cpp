#include "cascaded_shadow.h"

#include <stdexcept>

CascadedShadow::CascadedShadow(unsigned int width, unsigned int height, int numCascades)
    : width(width), height(height), numCascades(numCascades) {}

CascadedShadow::~CascadedShadow() {
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

void CascadedShadow::createVulkanResources(VkDevice device, VkPhysicalDevice physicalDevice, VkRenderPass renderPass, uint32_t w, uint32_t h, int numCascades) {
    width = w;
    height = h;
    this->numCascades = numCascades;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    vkShadowImages.resize(numCascades);
    vkShadowMemories.resize(numCascades);
    vkShadowViews.resize(numCascades);
    vkShadowFramebuffers.resize(numCascades);
    for (int i = 0; i < numCascades; ++i) {
        // Imagen depth
        VkImageCreateInfo imgInfo{};
        imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.extent.width = w;
        imgInfo.extent.height = h;
        imgInfo.extent.depth = 1;
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.format = depthFormat;
        imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imgInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device, &imgInfo, nullptr, &vkShadowImages[i]) != VK_SUCCESS)
            throw std::runtime_error("Failed to create cascaded shadow image");
        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(device, vkShadowImages[i], &memReq);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device, &allocInfo, nullptr, &vkShadowMemories[i]) != VK_SUCCESS)
            throw std::runtime_error("Failed to allocate cascaded shadow memory");
        vkBindImageMemory(device, vkShadowImages[i], vkShadowMemories[i], 0);
        // View
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = vkShadowImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = depthFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &viewInfo, nullptr, &vkShadowViews[i]) != VK_SUCCESS)
            throw std::runtime_error("Failed to create cascaded shadow view");
        // Framebuffer
        VkImageView attachments[] = {vkShadowViews[i]};
        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = renderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = attachments;
        fbInfo.width = w;
        fbInfo.height = h;
        fbInfo.layers = 1;
        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &vkShadowFramebuffers[i]) != VK_SUCCESS)
            throw std::runtime_error("Failed to create cascaded shadow framebuffer");
    }
}

void CascadedShadow::destroyVulkanResources(VkDevice device) {
    for (size_t i = 0; i < vkShadowFramebuffers.size(); ++i) {
        if (vkShadowFramebuffers[i] != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, vkShadowFramebuffers[i], nullptr);
            vkShadowFramebuffers[i] = VK_NULL_HANDLE;
        }
    }
    for (size_t i = 0; i < vkShadowViews.size(); ++i) {
        if (vkShadowViews[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(device, vkShadowViews[i], nullptr);
            vkShadowViews[i] = VK_NULL_HANDLE;
        }
    }
    for (size_t i = 0; i < vkShadowImages.size(); ++i) {
        if (vkShadowImages[i] != VK_NULL_HANDLE) {
            vkDestroyImage(device, vkShadowImages[i], nullptr);
            vkShadowImages[i] = VK_NULL_HANDLE;
        }
    }
    for (size_t i = 0; i < vkShadowMemories.size(); ++i) {
        if (vkShadowMemories[i] != VK_NULL_HANDLE) {
            vkFreeMemory(device, vkShadowMemories[i], nullptr);
            vkShadowMemories[i] = VK_NULL_HANDLE;
        }
    }
    vkShadowFramebuffers.clear();
    vkShadowViews.clear();
    vkShadowImages.clear();
    vkShadowMemories.clear();
}
