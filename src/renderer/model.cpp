#include "model.h"

Model::Model(const std::string& path) {
    // Cargar modelo y preparar recursos Vulkan
}

Model::~Model() {
    // Liberar recursos Vulkan asociados a los meshes
}

void Model::draw(VkCommandBuffer cmd) const {
    for (const auto& mesh : meshes) {
        mesh.draw(cmd);
    }
}

int Model::getVertexCount() const {
    int total = 0;
    for (const auto& mesh : meshes) total += mesh.getVertexCount();
    return total;
}

int Model::getTriangleCount() const {
    int total = 0;
    for (const auto& mesh : meshes) total += mesh.getTriangleCount();
    return total;
}