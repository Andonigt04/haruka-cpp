#ifndef RENDER_TARGET_H
#define RENDER_TARGET_H

#include <vulkan/vulkan.h>

class RenderTarget {
public:
    RenderTarget(unsigned int width, unsigned int height);
    ~RenderTarget();

    // Vulkan only
    void createVulkanResources(VkDevice device, VkPhysicalDevice physicalDevice, VkRenderPass renderPass, uint32_t w, uint32_t h);
    void destroyVulkanResources();
    
    VkFramebuffer vkFramebuffer = VK_NULL_HANDLE;
    VkImage vkColorImage = VK_NULL_HANDLE;
    VkDeviceMemory vkColorImageMemory = VK_NULL_HANDLE;
    VkImageView vkColorImageView = VK_NULL_HANDLE;
    VkImage vkDepthImage = VK_NULL_HANDLE;
    VkDeviceMemory vkDepthImageMemory = VK_NULL_HANDLE;
    VkImageView vkDepthImageView = VK_NULL_HANDLE;
    unsigned int width, height;
    VkDevice m_device = VK_NULL_HANDLE;
};

#endif