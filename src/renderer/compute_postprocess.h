#ifndef COMPUTEPOSTPROCESS_H
#define COMPUTEPOSTPROCESS_H

#include <vulkan/vulkan.h>
#include <vector>

/**
 * @brief Compute-shader based post-processing pipeline.
 *
 * Supports bloom, tone mapping, and color grading passes using SSBO-backed compute workloads.
 */

class ComputePostprocess {
public:
    ComputePostprocess(unsigned int width, unsigned int height);
    ~ComputePostprocess();

    // Vulkan only
    void createVulkanResources(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t w, uint32_t h);
    void destroyVulkanResources(VkDevice device);

    VkImage vkImage = VK_NULL_HANDLE;
    VkDeviceMemory vkImageMemory = VK_NULL_HANDLE;
    VkImageView vkImageView = VK_NULL_HANDLE;
    unsigned int width, height;

    // TODO: Implementar lógica de compute pipeline y recursos Vulkan
};

#endif