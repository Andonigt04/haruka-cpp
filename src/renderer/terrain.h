#ifndef TERRAIN_H
#define TERRAIN_H

#include <vulkan/vulkan.h>
#include <vector>
#include <glm/glm.hpp>

class Terrain {
public:
    Terrain(unsigned int width, unsigned int height);
    ~Terrain();

    // Vulkan only
    VkBuffer vkVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vkVertexMemory = VK_NULL_HANDLE;
    VkBuffer vkIndexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vkIndexMemory = VK_NULL_HANDLE;
    size_t indexCount = 0;
    unsigned int width, height;
    // Renderiza el terreno usando Vulkan
    void render(class Shader& shader, const class Camera* camera, VkCommandBuffer cmd) const;
    // TODO: Implementar lógica de creación y destrucción de buffers y recursos Vulkan para terreno
};

#endif