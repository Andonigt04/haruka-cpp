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
#include "rhi/rhi_types.h"

namespace Haruka { namespace RHI { class Context; } }

namespace Haruka { namespace Renderer {

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
    unsigned int VAO = 0;

    /** @brief Constructs mesh from full vertex/material data. */
    Mesh(std::vector<Vertex> vertex, std::vector<unsigned int> idx, std::vector<MeshTexture> textures);

    /** @brief Constructs mesh from simple geometry buffers. */
    Mesh(const std::vector<glm::vec3>& vertices,
         const std::vector<glm::vec3>& normals,
         const std::vector<unsigned int>& indices);

    ~Mesh();

    // Posee handles GL → NO copiable (una copia compartiría VAO/VBO y el destructor del
    // temporal los borraría → "non-gen name" al hacer bind). Solo MOVIBLE: roba los handles.
    Mesh(const Mesh&)            = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&& o) noexcept;
    Mesh& operator=(Mesh&& o) noexcept;

    /** @brief Issues draw call using associated textures and shader bindings. */
    void Draw(Shader &shader);
    /** @brief Compatibility alias for simple draw usage. */
    void draw() const;  // Alias para compatibilidad (ruta GL directa)

    /** @brief Dibuja por la ruta PSO/Context. El PIPELINE lo bindea el LLAMADOR (es quien elige
     *  shader + estado del pase); la malla solo aporta su geometría. Sustituye a VAO+glDrawElements
     *  por bindVertexBuffer/bindIndexBuffer/drawIndexed. Así se rompe el acoplamiento
     *  "Shader externo + VAO por geometría" que no existe en Vulkan. */
    void drawRHI(Haruka::RHI::Context& ctx) const;
    /** @brief Returns index count in index buffer. */
    size_t getIndexCount() const { return index.size(); }
    /** @brief Returns vertex count. Toda la geometría (completa o simple) vive ya en el mismo
     *  buffer interleaved → un único contador. */
    int getVertexCount() const { return static_cast<int>(vertex.size()); }
    /** @brief Returns triangle count (`indices / 3`). */
    int getTriangleCount() const { return static_cast<int>(index.size() / 3); }

    /** @brief Handles RHI de la geometría — la ruta PSO dibuja con
     *  bindVertexBuffer/bindIndexBuffer/drawIndexed en vez de VAO+glDrawElements. */
    Haruka::RHI::BufferHandle vertexBuffer() const { return m_vbo; }
    Haruka::RHI::BufferHandle indexBuffer()  const { return m_ebo; }

private:
    unsigned int VBO = 0, EBO = 0;    // ids GL nativos (cache; el owner es el device en ruta RHI)
    // Handles RHI de los buffers. (m_nbo eliminado: la geometría simple ya no usa un segundo
    // buffer de normales — va interleaved en m_vbo, ver setupSimpleMesh.)
    Haruka::RHI::BufferHandle m_vbo, m_ebo;
    bool isSimpleGeometry = false;

    /** @brief Configures VAO/VBO/EBO for full vertex path. */
    void setupMesh();
    /** @brief Configures buffers for simplified geometry path. */
    void setupSimpleMesh(const std::vector<glm::vec3>& vertices,
                         const std::vector<glm::vec3>& normals,
                         const std::vector<unsigned int>& indices);
};

}} // namespace Haruka::Renderer

// Back-compat aliases during the namespace migration.
using Haruka::Renderer::Vertex;
using Haruka::Renderer::MeshTexture;
using Haruka::Renderer::Mesh;
namespace Haruka { using Renderer::Vertex; using Renderer::MeshTexture; using Renderer::Mesh; }
#endif