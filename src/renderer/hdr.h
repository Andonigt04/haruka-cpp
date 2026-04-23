#pragma once

#include <vulkan/vulkan.h>

class HDR
{
public:
    HDR(unsigned int width, unsigned int height);
    ~HDR();

    // Vulkan only
    void createVulkanResources(VkDevice device, VkPhysicalDevice physicalDevice, VkRenderPass renderPass, uint32_t w, uint32_t h);
    void destroyVulkanResources(VkDevice device);

    VkImage vkImage = VK_NULL_HANDLE;
    VkDeviceMemory vkImageMemory = VK_NULL_HANDLE;
    VkImageView vkImageView = VK_NULL_HANDLE;
    VkFramebuffer vkFramebuffer = VK_NULL_HANDLE;
    unsigned int width, height;
};