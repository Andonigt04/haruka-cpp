#ifndef MESH_RENDERER_COMPONENT_H
#define MESH_RENDERER_COMPONENT_H

#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <string>

#include "renderer/simple_mesh.h"

class Shader;

/**
 * @brief Simple mesh renderer with source-data cache.
 *
 * Keeps a CPU copy of vertices/normals/indices for editing,
 * plus a resident `SimpleMesh` resource for drawing.
 */
class MeshRendererComponent {
public:
    MeshRendererComponent();
    ~MeshRendererComponent();

    /** @brief Replaces the current mesh and updates CPU/resident caches. */
    void setMesh(const std::vector<glm::vec3>& vertices, const std::vector<glm::vec3>& normals, const std::vector<unsigned int>& indices);

    /** @brief Releases the resident mesh from renderer/GPU memory. */
    void releaseMesh();
    /** @brief Returns true when a resident mesh is available. */
    bool isResident() const { return mesh != nullptr; }
    
    /** @brief Issues a draw call when a resident mesh exists. */
    void render(Shader& shader, VkCommandBuffer cmd) const;
    /** @brief Inspector ImGui de rutas auxiliares. */
    void renderInspector();
    
    /** @brief Returns shared resident mesh pointer. */
    std::shared_ptr<SimpleMesh> getMesh() const { return mesh; }
    /** @brief Cached vertex count of the last assigned mesh. */
    int getVertexCount() const { return cachedVertexCount; }
    /** @brief Cached triangle count of the last assigned mesh. */
    int getTriangleCount() const { return cachedTriangleCount; }
    /** @brief Resident vertex count, if mesh exists. */
    int getResidentVertexCount() const { return mesh ? mesh->getVertexCount() : 0; }
    /** @brief Resident triangle count, if mesh exists. */
    int getResidentTriangleCount() const { return mesh ? mesh->getTriangleCount() : 0; }
    /** @brief CPU source data for editing workflows. */
    const std::vector<glm::vec3>& getSourceVertices() const { return sourceVertices; }
    /** @brief CPU source normals. */
    const std::vector<glm::vec3>& getSourceNormals() const { return sourceNormals; }
    /** @brief CPU source indices. */
    const std::vector<unsigned int>& getSourceIndices() const { return sourceIndices; }

    /** @brief Returns the bounding sphere radius of the mesh (world units). */
    double getBoundingRadius() const;
    
private:
    std::shared_ptr<SimpleMesh> mesh;
    std::string meshPath;
    std::string materialPath;
    int cachedVertexCount = 0;
    int cachedTriangleCount = 0;
    std::vector<glm::vec3> sourceVertices;
    std::vector<glm::vec3> sourceNormals;
    std::vector<unsigned int> sourceIndices;
};

#endif