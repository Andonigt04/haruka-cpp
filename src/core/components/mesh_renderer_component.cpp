#include "mesh_renderer_component.h"
#include "renderer/shader.h"
#include "renderer/simple_mesh.h"
#include "imgui.h"

MeshRendererComponent::MeshRendererComponent() 
    : mesh(nullptr), meshPath(""), materialPath("") {}

MeshRendererComponent::~MeshRendererComponent() = default;

// Actualiza tanto recurso residente como copia fuente en CPU.
void MeshRendererComponent::setMesh(const std::vector<glm::vec3>& vertices,
                                    const std::vector<glm::vec3>& normals,
                                    const std::vector<glm::vec3>& colors,
                                    const std::vector<unsigned int>& indices) {
    mesh = std::make_shared<SimpleMesh>(vertices, normals, indices);
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
void MeshRendererComponent::render(Shader& shader) const {
    if (mesh) {
        mesh->draw();
    }
}

// Herramientas editor-only para inspección/edición de rutas asociadas.
void MeshRendererComponent::renderInspector() {
    ImGui::Text("Mesh Renderer");
    char meshBuffer[128] = {};
    strcpy(meshBuffer, meshPath.c_str());
    if (ImGui::InputText("Mesh##path", meshBuffer, 128)) {
        meshPath = meshBuffer;
    }
    
    char matBuffer[128] = {};
    strcpy(matBuffer, materialPath.c_str());
    if (ImGui::InputText("Material##path", matBuffer, 128)) {
        materialPath = matBuffer;
    }
}