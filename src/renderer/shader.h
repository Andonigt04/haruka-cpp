#ifndef SHADER_H
#define SHADER_H

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <string>

class Shader {
public:
    // Vulkan only
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    VkPipeline vkPipeline = VK_NULL_HANDLE;
    VkPipelineLayout vkPipelineLayout = VK_NULL_HANDLE;

    std::string vertexPath;
    std::string fragmentPath;

    Shader(const char* vertexPath, const char* fragmentPath);
    ~Shader();

    // Métodos para crear/destroy pipeline y shader modules Vulkan
    void createVulkanPipeline(VkDevice device, VkRenderPass renderPass, VkDescriptorSetLayout* setLayouts, uint32_t setLayoutCount);
    void destroyVulkanPipeline(VkDevice device);
};

#endif