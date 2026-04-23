#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <glm/glm.hpp>

class CascadedShadow {
public:
    CascadedShadow(unsigned int width, unsigned int height, int numCascades);
    ~CascadedShadow();

    // Vulkan only
    void createVulkanResources(VkDevice device, VkPhysicalDevice physicalDevice, VkRenderPass renderPass, uint32_t w, uint32_t h, int numCascades);
    void destroyVulkanResources(VkDevice device);

    std::vector<VkImage> vkShadowImages;
    std::vector<VkDeviceMemory> vkShadowMemories;
    std::vector<VkImageView> vkShadowViews;
    std::vector<VkFramebuffer> vkShadowFramebuffers;
    unsigned int width, height;
    int numCascades;
    // TODO: Implementar lógica de actualización de matrices y recursos Vulkan
};
