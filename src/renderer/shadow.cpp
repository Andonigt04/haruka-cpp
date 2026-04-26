#include "shadow.h"

#include <iostream>

#include "core/error_reporter.h"

Shadow::Shadow(unsigned int width, unsigned int height)
    : shadowWidth(width), shadowHeight(height) {}



Shadow::~Shadow() {
    destroyResources();
}

void Shadow::createResources(VkDevice device_, VkPhysicalDevice physicalDevice, VkRenderPass renderPass_) {
    device = device_;
    renderPass = renderPass_;
    // Crear imagen de profundidad
    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.extent.width = shadowWidth;
    imgInfo.extent.height = shadowHeight;
    imgInfo.extent.depth = 1;
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.format = VK_FORMAT_D32_SFLOAT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imgInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateImage(device, &imgInfo, nullptr, &depthImage);
    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, depthImage, &memReq);
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
    vkAllocateMemory(device, &allocInfo, nullptr, &depthImageMemory);
    vkBindImageMemory(device, depthImage, depthImageMemory, 0);
    // Crear image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = depthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_D32_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    vkCreateImageView(device, &viewInfo, nullptr, &depthImageView);
    // Crear framebuffer
    VkImageView attachments[] = { depthImageView };
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = attachments;
    fbInfo.width = shadowWidth;
    fbInfo.height = shadowHeight;
    fbInfo.layers = 1;
    vkCreateFramebuffer(device, &fbInfo, nullptr, &depthFramebuffer);
}

void Shadow::destroyResources() {
    if (depthFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, depthFramebuffer, nullptr);
        depthFramebuffer = VK_NULL_HANDLE;
    }
    if (depthImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, depthImageView, nullptr);
        depthImageView = VK_NULL_HANDLE;
    }
    if (depthImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, depthImage, nullptr);
        depthImage = VK_NULL_HANDLE;
    }
    if (depthImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, depthImageMemory, nullptr);
        depthImageMemory = VK_NULL_HANDLE;
    }
}

void Shadow::bindForWriting(VkCommandBuffer cmdBuffer) {
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass;
    renderPassInfo.framebuffer = depthFramebuffer;
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {shadowWidth, shadowHeight};
    VkClearValue clearValue{};
    clearValue.depthStencil = {1.0f, 0};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;
    vkCmdBeginRenderPass(cmdBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
}

void Shadow::bindForReading(VkCommandBuffer cmdBuffer, unsigned int textureUnit) {
    // En Vulkan, el bind para lectura se hace en el descriptor set layout del shader.
    // Aquí solo se expone depthImageView para que el usuario lo use en el descriptor.
    // El usuario debe actualizar el descriptor set con depthImageView y un sampler adecuado.
    (void)cmdBuffer;
    (void)textureUnit;
}

void Shadow::unbind(VkCommandBuffer cmdBuffer) {
    vkCmdEndRenderPass(cmdBuffer);
}

void Shadow::setupFramebuffer() {
    // TODO: Crear framebuffer y recursos Vulkan para shadow map
}
