#include "MeshRendererComponent.h"
#include <cstdio>
#include "Haruka/Renderer/Shader.h"
#include "Haruka/Renderer/SimpleMesh.h"
#include "Haruka/RHI/RHIDevice.h"
#include "imgui.h"

namespace Haruka::Core::Components {

MeshRendererComponent::MeshRendererComponent()
    : mesh(nullptr), meshPath(""), materialPath("") {}

MeshRendererComponent::~MeshRendererComponent() = default;

// Actualiza tanto recurso residente como copia fuente en CPU.
void MeshRendererComponent::setMesh(const std::vector<glm::vec3>& vertices,
                                    const std::vector<glm::vec3>& normals,
                                    const std::vector<glm::vec3>& colors,
                                    const std::vector<unsigned int>& indices) {
    mesh = std::make_shared<Renderer::Mesh>(vertices, normals, indices);
    sourceVertices = vertices;
    sourceNormals = normals;
    sourceColors = colors;
    sourceIndices = indices;
    cachedVertexCount = static_cast<int>(vertices.size());
    cachedTriangleCount = static_cast<int>(indices.size() / 3);
}

// Libera sólo el recurso de malla residente.
void MeshRendererComponent::releaseMesh() {
    mesh.reset();
}

// Dibuja únicamente cuando existe malla válida.
void MeshRendererComponent::render(Renderer::Shader& shader) const {
    if (mesh) {
        if (Haruka::RHI::Device* dev = Haruka::RHI::device())
            mesh->drawRHI(*dev->beginFrame());
    }
}

void MeshRendererComponent::renderRHI(Haruka::RHI::Context& ctx) const {
    if (mesh) mesh->drawRHI(ctx);
}

// Herramientas editor-only para inspección/edición de rutas asociadas.
void MeshRendererComponent::renderInspector() {
    ImGui::Text("Mesh Renderer");
    char meshBuffer[128] = {};
    std::snprintf(meshBuffer, sizeof(meshBuffer), "%s", meshPath.c_str());   // strcpy desbordaba con rutas de >=128
    if (ImGui::InputText("Mesh##path", meshBuffer, 128)) {
        meshPath = meshBuffer;
    }
    
    char matBuffer[128] = {};
    std::snprintf(matBuffer, sizeof(matBuffer), "%s", materialPath.c_str());   // strcpy desbordaba con rutas de >=128
    if (ImGui::InputText("Material##path", matBuffer, 128)) {
        materialPath = matBuffer;
    }
}
} // namespace Haruka::Core::Components
