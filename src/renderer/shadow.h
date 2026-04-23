#ifndef SHADOW_H
#define SHADOW_H

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

/**
 * @brief Directional shadow map wrapper.
 *
 * Owns a depth framebuffer and a single depth texture.
 */
class Shadow
{
public:
    // Vulkan shadow map resources
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthImageMemory = VK_NULL_HANDLE;
    VkImageView depthImageView = VK_NULL_HANDLE;
    VkFramebuffer depthFramebuffer = VK_NULL_HANDLE;
    unsigned int shadowWidth, shadowHeight;
    VkDevice device = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;

    /** @brief Creates a shadow map with the requested resolution. */
    Shadow(unsigned int width = 1024, unsigned int height = 1024);
    ~Shadow();
    void createResources(VkDevice device, VkPhysicalDevice physicalDevice, VkRenderPass renderPass);
    void destroyResources();

    /** @brief Binds the framebuffer for shadow depth rendering. */
    // Inicia el render pass de sombras en el command buffer dado
    void bindForWriting(VkCommandBuffer cmdBuffer);
    void bindForReading(VkCommandBuffer cmdBuffer, unsigned int textureUnit);
    // Devuelve el image view para bindear en el descriptor set
    VkImageView getDepthImageView() const { return depthImageView; }
    // Termina el render pass de sombras
    void unbind(VkCommandBuffer cmdBuffer);
private:
    /** @brief Allocates FBO/texture objects. */
    void setupFramebuffer();
};
#endif