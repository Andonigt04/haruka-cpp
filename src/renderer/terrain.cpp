
#include "terrain.h"

Terrain::Terrain(unsigned int width, unsigned int height)
    : width(width), height(height) {}

Terrain::~Terrain() {
    // Vulkan: liberar recursos debe llamarse explícitamente
}

void Terrain::render(class Shader& shader, const class Camera* camera, VkCommandBuffer cmd) const {
    // TODO: Implementar renderizado de terreno con Vulkan
    // vkCmdBindPipeline(cmd, ...);
    // vkCmdBindVertexBuffers(cmd, ...);
    // vkCmdBindIndexBuffer(cmd, ...);
    // vkCmdDrawIndexed(cmd, ...);
}

// TODO: Implementar creación y destrucción de buffers y recursos Vulkan para terreno
