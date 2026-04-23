#ifndef MESH_H
#define MESH_H

#include <iostream>
#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "shader.h"
#include "buffer_objects.h"

#pragma pack(push, 1)
struct Vertex {
    glm::vec3 Position;
    glm::vec3 Normal;
    glm::vec2 TexCoords;
    glm::vec3 Tangent;
    glm::vec3 Bitangent;
};
#pragma pack(pop)

struct MeshTexture {
    VkImage vkImage = VK_NULL_HANDLE;
    VkDeviceMemory vkImageMemory = VK_NULL_HANDLE;
    VkImageView vkImageView = VK_NULL_HANDLE;
    VkSampler vkSampler = VK_NULL_HANDLE;
    std::string type;
    std::string path;
    void createVulkanImage(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t width, uint32_t height, const void* pixels);
    void destroyVulkanImage(VkDevice device);
    VkDescriptorSet vkDescriptorSet = VK_NULL_HANDLE;
};

class Mesh {
public:
    std::vector<Vertex>       vertex;
    std::vector<unsigned int> index;
    std::vector<MeshTexture>  textures;
    // Vulkan only
    VertexBuffer vertexBuffer;
    IndexBuffer indexBuffer;

    Mesh(std::vector<Vertex> vertex, std::vector<unsigned int> idx, std::vector<MeshTexture> textures);
    Mesh(const std::vector<glm::vec3>& vertices,
         const std::vector<glm::vec3>& normals,
         const std::vector<unsigned int>& indices);
    ~Mesh();

    void createVulkanBuffers(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue transferQueue, VkCommandPool commandPool);
    void destroyVulkanBuffers();
    // Vulkan draw call
    void draw(VkCommandBuffer cmd) const;
    
    size_t getIndexCount() const { return index.size(); }
    int getVertexCount() const { return static_cast<int>(vertex.size()); }
    int getTriangleCount() const { return static_cast<int>(index.size() / 3); }
private:
    // Vulkan only
};

#endif