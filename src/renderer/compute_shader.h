#ifndef COMPUTE_SHADER_H
#define COMPUTE_SHADER_H

#include <vulkan/vulkan.h>
#include <vector>

class ComputeShader
{
public:
    ComputeShader(const char* computePath);
    ~ComputeShader();

    // Vulkan only
    VkShaderModule computeModule = VK_NULL_HANDLE;
    VkPipeline computePipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;

    void createVulkanPipeline(VkDevice device, const char* computePath, VkDescriptorSetLayout* setLayouts, uint32_t setLayoutCount);
    void destroyVulkanPipeline(VkDevice device);
};
#endif