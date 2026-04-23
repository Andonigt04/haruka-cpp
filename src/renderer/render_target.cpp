#include "render_target.h"

#include <stdexcept>

RenderTarget::RenderTarget(unsigned int width, unsigned int height)
    : width(width), height(height) {}

RenderTarget::~RenderTarget() {
    destroyVulkanResources();
}

void RenderTarget::createVulkanResources(VkDevice device, VkPhysicalDevice physicalDevice, VkRenderPass renderPass, uint32_t w, uint32_t h) {
    m_device = device;
}

void RenderTarget::destroyVulkanResources() {
    if (vkFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(m_device, vkFramebuffer, nullptr);
        vkFramebuffer = VK_NULL_HANDLE;
    }
    if (vkColorImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, vkColorImageView, nullptr);
        vkColorImageView = VK_NULL_HANDLE;
    }
    if (vkColorImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, vkColorImage, nullptr);
        vkColorImage = VK_NULL_HANDLE;
    }
    if (vkColorImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, vkColorImageMemory, nullptr);
        vkColorImageMemory = VK_NULL_HANDLE;
    }
    if (vkDepthImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, vkDepthImageView, nullptr);
        vkDepthImageView = VK_NULL_HANDLE;
    }
    if (vkDepthImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, vkDepthImage, nullptr);
        vkDepthImage = VK_NULL_HANDLE;
    }
    if (vkDepthImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, vkDepthImageMemory, nullptr);
        vkDepthImageMemory = VK_NULL_HANDLE;
    }
}