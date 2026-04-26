#ifndef VIRTUAL_TEXTURING_H
#define VIRTUAL_TEXTURING_H

#include <vulkan/vulkan.h>
#include <vector>
#include <string>

class VirtualTexturing {
public:
    VirtualTexturing(unsigned int width, unsigned int height);
    ~VirtualTexturing();

    // Vulkan only
    std::vector<VkImage> vkVirtualImages;
    std::vector<VkDeviceMemory> vkVirtualMemories;
    std::vector<VkImageView> vkVirtualViews;
    unsigned int width, height;

    void createVulkanResources(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t numPages);
    void destroyVulkanResources(VkDevice device);
};

#endif