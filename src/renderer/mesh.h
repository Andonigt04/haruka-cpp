#ifndef MESH_H
#define MESH_H

#include <vector>
#include <string>
#include <glm/glm.hpp>
#include "rhi/rhi_types.h"

namespace Haruka { namespace RHI { class Context; } }

namespace Haruka { namespace Renderer {

#pragma pack(push, 1)
struct Vertex {
    glm::vec3 Position;
    glm::vec3 Normal;
    glm::vec2 TexCoords;
    glm::vec3 Tangent;
    glm::vec3 Bitangent;
};
#pragma pack(pop)

struct MeshTexture {
    unsigned int id;
    Haruka::RHI::TextureHandle rhiHandle;
    std::string type;
    std::string path;
};

class Mesh {
public:
    std::vector<Vertex>       vertex;
    std::vector<unsigned int> index;
    std::vector<MeshTexture>  textures;

    Mesh(std::vector<Vertex> vertex, std::vector<unsigned int> idx, std::vector<MeshTexture> textures);
    Mesh(const std::vector<glm::vec3>& vertices,
         const std::vector<glm::vec3>& normals,
         const std::vector<unsigned int>& indices);
    ~Mesh();

    Mesh(const Mesh&)            = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&& o) noexcept;
    Mesh& operator=(Mesh&& o) noexcept;

    void drawRHI(Haruka::RHI::Context& ctx) const;
    size_t getIndexCount() const { return index.size(); }
    int getVertexCount() const { return static_cast<int>(vertex.size()); }
    int getTriangleCount() const { return static_cast<int>(index.size() / 3); }

    Haruka::RHI::BufferHandle vertexBuffer() const { return m_vbo; }
    Haruka::RHI::BufferHandle indexBuffer()  const { return m_ebo; }

private:
    Haruka::RHI::BufferHandle m_vbo, m_ebo;
    bool isSimpleGeometry = false;

    void setupMesh();
    void setupSimpleMesh(const std::vector<glm::vec3>& vertices,
                         const std::vector<glm::vec3>& normals,
                         const std::vector<unsigned int>& indices);
};

}} // namespace Haruka::Renderer

using Haruka::Renderer::Vertex;
using Haruka::Renderer::MeshTexture;
using Haruka::Renderer::Mesh;
namespace Haruka { using Renderer::Vertex; using Renderer::MeshTexture; using Renderer::Mesh; }
#endif
