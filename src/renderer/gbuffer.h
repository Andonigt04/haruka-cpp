#ifndef GBUFFER_H
#define GBUFFER_H

#include <vulkan/vulkan.h>
#include <vector>

class GBuffer {
public:
    GBuffer(unsigned int width, unsigned int height);
    ~GBuffer();

    // Vulkan only
    void createVulkanResources(VkDevice device, VkPhysicalDevice physicalDevice, VkRenderPass renderPass, uint32_t w, uint32_t h);
    void destroyVulkanResources(VkDevice device);

    std::vector<VkImage> vkImages;
    std::vector<VkDeviceMemory> vkImageMemories;
    std::vector<VkImageView> vkImageViews;
    VkFramebuffer vkFramebuffer = VK_NULL_HANDLE;
    unsigned int width, height;
};

#endif