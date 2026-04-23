#include "simple_mesh.h"

SimpleMesh::SimpleMesh(const std::vector<glm::vec3>& vertices, const std::vector<unsigned int>& indices) {
    // Crear buffers Vulkan para los vértices e índices
    indexCount = indices.size();
    vertexCount = vertices.size();
}

SimpleMesh::~SimpleMesh() {
    // Liberar buffers Vulkan
}

void SimpleMesh::draw(VkCommandBuffer cmd) const {
    if (vkVertexBuffer == VK_NULL_HANDLE || vkIndexBuffer == VK_NULL_HANDLE || indexCount == 0) return;
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &vkVertexBuffer, offsets);
    vkCmdBindIndexBuffer(cmd, vkIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, static_cast<uint32_t>(indexCount), 1, 0, 0, 0);
}