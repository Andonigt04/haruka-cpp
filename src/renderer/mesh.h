/**
 * @file mesh.h
 * @brief GPU mesh wrapper with full-vertex and simple-geometry construction paths.
 */
#ifndef MESH_H
#define MESH_H

#include <iostream>
#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <glad/glad.h>
#include "shader.h"

/** @brief Interleaved vertex layout used by complex mesh path. */
#pragma pack(push, 1)
struct Vertex {
    glm::vec3 Position;
    glm::vec3 Normal;
    glm::vec2 TexCoords;
    glm::vec3 Tangent;
    glm::vec3 Bitangent;
};
#pragma pack(pop)

/** @brief Texture binding descriptor attached to a mesh material slot. */
struct MeshTexture {
    unsigned int id;
    std::string type;
    std::string path;
};

/**
 * @brief GPU mesh wrapper with two construction paths.
 *
 * Supports:
 * - full model path (`Vertex` + textures)
 * - simplified geometry path (positions/normals/indices)
 */
class Mesh {
public:
    std::vector<Vertex>       vertex;
    std::vector<unsigned int> index;
    std::vector<MeshTexture>  textures;
    unsigned int VAO;

    /** @brief Constructs mesh from full vertex/material data. */
    Mesh(std::vector<Vertex> vertex, std::vector<unsigned int> idx, std::vector<MeshTexture> textures);
    
    /** @brief Constructs mesh from simple geometry buffers. */
    Mesh(const std::vector<glm::vec3>& vertices,
         const std::vector<glm::vec3>& normals,
         const std::vector<unsigned int>& indices);
    
    ~Mesh();

    /** @brief Issues draw call using associated textures and shader bindings. */
    void Draw(Shader &shader);
    /** @brief Compatibility alias for simple draw usage. */
    void draw() const;  // Alias para compatibilidad
    /** @brief Returns index count in index buffer. */
    size_t getIndexCount() const { return index.size(); }
    /** @brief Returns vertex count for active mesh representation. */
    int getVertexCount() const { return isSimpleGeometry ? simpleVertexCount : static_cast<int>(vertex.size()); }
    /** @brief Returns triangle count (`indices / 3`). */
    int getTriangleCount() const { return static_cast<int>(index.size() / 3); }

private:
    unsigned int VBO, EBO;
    GLuint nbo = 0;  // Normal buffer para geometria simple
    bool isSimpleGeometry = false;
    int simpleVertexCount = 0;
    
    /** @brief Configures VAO/VBO/EBO for full vertex path. */
    void setupMesh();
    /** @brief Configures buffers for simplified geometry path. */
    void setupSimpleMesh(const std::vector<glm::vec3>& vertices,
                         const std::vector<glm::vec3>& normals,
                         const std::vector<unsigned int>& indices);
};

#endif