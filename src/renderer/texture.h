#ifndef TEXTURE_H
#define TEXTURE_H

#include <vulkan/vulkan.h>
#include <string>
#include <cstdint>

class Texture {
public:
    Texture() = default;
    Texture(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue transferQueue, VkCommandPool commandPool, const std::string& path);
    ~Texture();

    // No copiar
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    // Permitir mover
    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;

    void create(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue transferQueue, VkCommandPool commandPool, const std::string& path);
    void destroy();

    VkImage vkImage = VK_NULL_HANDLE;
    VkDeviceMemory vkImageMemory = VK_NULL_HANDLE;
    VkImageView vkImageView = VK_NULL_HANDLE;
    VkSampler vkSampler = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;

private:
    VkDevice m_device = VK_NULL_HANDLE;
};
#endif