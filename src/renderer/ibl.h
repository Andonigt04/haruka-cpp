#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <string>

/**
 * @brief Image-based lighting precomputation helper.
 *
 * Manages environment cubemap generation, irradiance, prefilter, and BRDF LUT.
 */
class IBL {

public:
    IBL();
    ~IBL();

    // Vulkan: recursos para cubemaps y LUTs
    VkImage envCubemapImage = VK_NULL_HANDLE;
    VkImageView envCubemapView = VK_NULL_HANDLE;
    VkSampler envCubemapSampler = VK_NULL_HANDLE;

    VkImage irradianceImage = VK_NULL_HANDLE;
    VkDeviceMemory irradianceImageMemory = VK_NULL_HANDLE;
    VkImageView irradianceView = VK_NULL_HANDLE;
    VkSampler irradianceSampler = VK_NULL_HANDLE;

    VkImage prefilterImage = VK_NULL_HANDLE;
    VkDeviceMemory prefilterImageMemory = VK_NULL_HANDLE;
    VkImageView prefilterView = VK_NULL_HANDLE;
    VkSampler prefilterSampler = VK_NULL_HANDLE;

    VkImage brdfLUTImage = VK_NULL_HANDLE;
    VkDeviceMemory brdfLUTImageMemory = VK_NULL_HANDLE;
    VkImageView brdfLUTView = VK_NULL_HANDLE;
    VkSampler brdfLUTSampler = VK_NULL_HANDLE;

    void createVulkanResources(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue transferQueue, VkCommandPool commandPool);
    void destroyVulkanResources(VkDevice device);

    // Métodos para cargar HDRI y generar mapas (a implementar)
    void loadHDRI(const std::string& imagePath);
    void generateIrradianceMap(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue, VkCommandPool commandPool, VkRenderPass renderPass, VkDescriptorSetLayout* setLayouts, uint32_t setLayoutCount);
    void generatePrefilterMap(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue, VkCommandPool commandPool, VkRenderPass renderPass, VkDescriptorSetLayout* setLayouts, uint32_t setLayoutCount);
    void generateBRDFLUT(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue, VkCommandPool commandPool, VkRenderPass renderPass, VkDescriptorSetLayout* setLayouts, uint32_t setLayoutCount);

    // Renderizado de primitivas para convolución y LUT
    void renderCube(VkCommandBuffer cmd);
    void renderQuad(VkCommandBuffer cmd);

    // Binds para recursos de prefilter y BRDF LUT
    void bindPrefilterMap(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t set, uint32_t binding);
    void bindBRDFLUT(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t set, uint32_t binding);

private:
    // Métodos auxiliares Vulkan (a implementar)
    void setupCubemap();
    void renderCube();
    void renderQuad();
    // TODO: Vulkan: reemplazar unsigned int por VkImage, VkImageView, VkBuffer
    // VkImage envCubemapImage, irradianceImage, prefilterImage, brdfLUTImage;
    // VkImageView envCubemapView, irradianceView, prefilterView, brdfLUTView;
    // VkBuffer cubeBuffer, quadBuffer;
};