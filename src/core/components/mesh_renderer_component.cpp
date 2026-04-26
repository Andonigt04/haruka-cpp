#include "mesh_renderer_component.h"

#include "imgui.h"

#include "renderer/shader.h"
#include "renderer/simple_mesh.h"

MeshRendererComponent::MeshRendererComponent() 
    : mesh(nullptr), meshPath(""), materialPath("") {}

MeshRendererComponent::~MeshRendererComponent() = default;

// Actualiza tanto recurso residente como copia fuente en CPU.
void MeshRendererComponent::setMesh(const std::vector<glm::vec3>& vertices,
                                    const std::vector<glm::vec3>& normals,
                                    const std::vector<unsigned int>& indices) {
    mesh = std::make_shared<SimpleMesh>(vertices, indices);
    sourceVertices = vertices;
    sourceNormals = normals;
    sourceIndices = indices;
    cachedVertexCount = static_cast<int>(vertices.size());
    cachedTriangleCount = static_cast<int>(indices.size() / 3);
}

// Libera sólo el recurso de malla residente.
void MeshRendererComponent::releaseMesh() {
    mesh.reset();
}

// Dibuja únicamente cuando existe malla válida.
void MeshRendererComponent::render(Shader& shader, VkCommandBuffer cmd) const {
    if (mesh) mesh->draw(cmd);
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

// Returns the bounding sphere radius of the mesh (world units)
double MeshRendererComponent::getBoundingRadius() const {
    if (sourceVertices.empty()) return 1.0;
    glm::vec3 center(0.0f);
    for (const auto& v : sourceVertices) center += v;
    center /= static_cast<float>(sourceVertices.size());
    double maxDistSq = 0.0;
    for (const auto& v : sourceVertices) {
        double dist = glm::distance(center, v);
        double distSq = dist * dist;
        if (distSq > maxDistSq) maxDistSq = distSq;
    }
    return std::sqrt(maxDistSq);
}