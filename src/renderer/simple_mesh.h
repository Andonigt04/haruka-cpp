#ifndef SIMPLE_MESH_H
#define SIMPLE_MESH_H

#include <vulkan/vulkan.h>
#include <vector>
#include <glm/glm.hpp>

class SimpleMesh {
public:
    SimpleMesh(const std::vector<glm::vec3>& vertices, const std::vector<unsigned int>& indices);
    ~SimpleMesh();

    // Vulkan only
    VkBuffer vkVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vkVertexMemory = VK_NULL_HANDLE;
    VkBuffer vkIndexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vkIndexMemory = VK_NULL_HANDLE;
    size_t indexCount = 0;
    size_t vertexCount = 0;

    // Dibuja la malla usando Vulkan
    void draw(VkCommandBuffer cmd) const;

    // Métodos de consulta
    int getVertexCount() const { return static_cast<int>(vertexCount); }
    int getTriangleCount() const { return static_cast<int>(indexCount / 3); }
};

#endif