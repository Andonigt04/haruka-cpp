// Vulkan-only buffer objects
#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>

class VertexBuffer {
public:
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    size_t size = 0;

    VertexBuffer() = default;
    ~VertexBuffer();

    // Crea un buffer de vértices y sube los datos
    void create(VkDevice device, VkPhysicalDevice physicalDevice, const void* data, size_t size, VkQueue transferQueue, VkCommandPool commandPool);
    void destroy();
};

class IndexBuffer {
public:
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    size_t count = 0;

    IndexBuffer() = default;
    ~IndexBuffer();

    // Crea un buffer de índices y sube los datos
    void create(VkDevice device, VkPhysicalDevice physicalDevice, const uint32_t* indices, size_t count, VkQueue transferQueue, VkCommandPool commandPool);
    void destroy();
};

// En Vulkan no existe VAO, pero se pueden definir helpers para descripciones de input
struct VertexInputDescription {
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
};