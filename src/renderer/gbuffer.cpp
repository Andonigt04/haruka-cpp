#include "gbuffer.h"
#include <stdexcept>

GBuffer::GBuffer(unsigned int width, unsigned int height)
    : width(width), height(height) {}

GBuffer::~GBuffer() {
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

void GBuffer::createVulkanResources(VkDevice device, VkPhysicalDevice physicalDevice, VkRenderPass renderPass, uint32_t w, uint32_t h) {
    width = w;
    height = h;
    // Definir formatos de los attachments (posición, normal, albedo, depth)
    std::vector<VkFormat> formats = {
        VK_FORMAT_R16G16B16A16_SFLOAT, // Position
        VK_FORMAT_R16G16B16A16_SFLOAT, // Normal
        VK_FORMAT_R8G8B8A8_UNORM       // Albedo
    };
    size_t numAttachments = formats.size();
    vkImages.resize(numAttachments);
    vkImageMemories.resize(numAttachments);
    vkImageViews.resize(numAttachments);
    for (size_t i = 0; i < numAttachments; ++i) {
        VkImageCreateInfo imgInfo{};
        imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.extent.width = w;
        imgInfo.extent.height = h;
        imgInfo.extent.depth = 1;
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.format = formats[i];
        imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device, &imgInfo, nullptr, &vkImages[i]) != VK_SUCCESS)
            throw std::runtime_error("Failed to create GBuffer image");
        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(device, vkImages[i], &memReq);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device, &allocInfo, nullptr, &vkImageMemories[i]) != VK_SUCCESS)
            throw std::runtime_error("Failed to allocate GBuffer image memory");
        vkBindImageMemory(device, vkImages[i], vkImageMemories[i], 0);
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = vkImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = formats[i];
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &viewInfo, nullptr, &vkImageViews[i]) != VK_SUCCESS)
            throw std::runtime_error("Failed to create GBuffer image view");
    }
    // Depth attachment
    VkImage depthImage;
    VkDeviceMemory depthMemory;
    VkImageView depthView;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    VkImageCreateInfo depthImgInfo{};
    depthImgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depthImgInfo.imageType = VK_IMAGE_TYPE_2D;
    depthImgInfo.extent.width = w;
    depthImgInfo.extent.height = h;
    depthImgInfo.extent.depth = 1;
    depthImgInfo.mipLevels = 1;
    depthImgInfo.arrayLayers = 1;
    depthImgInfo.format = depthFormat;
    depthImgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthImgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthImgInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthImgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    depthImgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device, &depthImgInfo, nullptr, &depthImage) != VK_SUCCESS)
        throw std::runtime_error("Failed to create GBuffer depth image");
    VkMemoryRequirements depthMemReq;
    vkGetImageMemoryRequirements(device, depthImage, &depthMemReq);
    VkMemoryAllocateInfo depthAllocInfo{};
    depthAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    depthAllocInfo.allocationSize = depthMemReq.size;
    depthAllocInfo.memoryTypeIndex = findMemoryType(physicalDevice, depthMemReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &depthAllocInfo, nullptr, &depthMemory) != VK_SUCCESS)
        throw std::runtime_error("Failed to allocate GBuffer depth memory");
    vkBindImageMemory(device, depthImage, depthMemory, 0);
    VkImageViewCreateInfo depthViewInfo{};
    depthViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depthViewInfo.image = depthImage;
    depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthViewInfo.format = depthFormat;
    depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthViewInfo.subresourceRange.baseMipLevel = 0;
    depthViewInfo.subresourceRange.levelCount = 1;
    depthViewInfo.subresourceRange.baseArrayLayer = 0;
    depthViewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &depthViewInfo, nullptr, &depthView) != VK_SUCCESS)
        throw std::runtime_error("Failed to create GBuffer depth view");

    // Crear framebuffer
    std::vector<VkImageView> attachments = vkImageViews;
    attachments.push_back(depthView);
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = renderPass;
    fbInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    fbInfo.pAttachments = attachments.data();
    fbInfo.width = w;
    fbInfo.height = h;
    fbInfo.layers = 1;
    if (vkCreateFramebuffer(device, &fbInfo, nullptr, &vkFramebuffer) != VK_SUCCESS)
        throw std::runtime_error("Failed to create GBuffer framebuffer");

    // Guardar depth para destrucción
    vkImages.push_back(depthImage);
    vkImageMemories.push_back(depthMemory);
    vkImageViews.push_back(depthView);
}

void GBuffer::destroyVulkanResources(VkDevice device) {
    if (vkFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, vkFramebuffer, nullptr);
        vkFramebuffer = VK_NULL_HANDLE;
    }
    for (size_t i = 0; i < vkImageViews.size(); ++i) {
        if (vkImageViews[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(device, vkImageViews[i], nullptr);
            vkImageViews[i] = VK_NULL_HANDLE;
        }
    }
    for (size_t i = 0; i < vkImages.size(); ++i) {
        if (vkImages[i] != VK_NULL_HANDLE) {
            vkDestroyImage(device, vkImages[i], nullptr);
            vkImages[i] = VK_NULL_HANDLE;
        }
    }
    for (size_t i = 0; i < vkImageMemories.size(); ++i) {
        if (vkImageMemories[i] != VK_NULL_HANDLE) {
            vkFreeMemory(device, vkImageMemories[i], nullptr);
            vkImageMemories[i] = VK_NULL_HANDLE;
        }
    }
    vkImageViews.clear();
    vkImages.clear();
    vkImageMemories.clear();
}