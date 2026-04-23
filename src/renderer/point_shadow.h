#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <glm/glm.hpp>

class PointShadow {
public:
    PointShadow(unsigned int width, unsigned int height);
    ~PointShadow();

    // Vulkan only
    VkImage vkShadowImage = VK_NULL_HANDLE;
    VkDeviceMemory vkShadowMemory = VK_NULL_HANDLE;
    VkImageView vkShadowView = VK_NULL_HANDLE;
    VkFramebuffer vkShadowFramebuffer = VK_NULL_HANDLE;
    unsigned int width, height;

    void createVulkanResources(VkDevice device, VkPhysicalDevice physicalDevice, VkRenderPass renderPass, uint32_t w, uint32_t h);
    void destroyVulkanResources(VkDevice device);
};