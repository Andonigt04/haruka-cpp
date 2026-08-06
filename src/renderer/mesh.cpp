#include "mesh.h"

#include <utility>
#include <cmath>
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"
#include <cstdio>
#include "core/logger.h"

namespace Haruka { namespace Renderer {

Mesh::Mesh(std::vector<Vertex> vertex, std::vector<unsigned int> idx, std::vector<MeshTexture> textures) {
    if (vertex.empty()) {
        HARUKA_RENDERER_ERROR(ErrorCode::MODEL_LOAD_FAILED, "Empty vertex list when creating mesh");
        return;
    }
    this->vertex = vertex;
    this->index = idx;
    this->textures = textures;
    this->isSimpleGeometry = false;
    setupMesh();
}

Mesh::Mesh(const std::vector<glm::vec3>& vertices,
           const std::vector<glm::vec3>& normals,
           const std::vector<unsigned int>& indices) {
    this->isSimpleGeometry = true;
    this->textures.clear();
    setupSimpleMesh(vertices, normals, indices);
}

static void releaseMeshBuffers(Haruka::RHI::BufferHandle vbo, Haruka::RHI::BufferHandle ebo) {
    using namespace Haruka;
    if (RHI::Device* dev = RHI::device()) {
        if (RHI::valid(vbo)) dev->destroy(vbo);
        if (RHI::valid(ebo)) dev->destroy(ebo);
    }
}

Mesh::~Mesh() {
    releaseMeshBuffers(m_vbo, m_ebo);
}

Mesh::Mesh(Mesh&& o) noexcept
    : vertex(std::move(o.vertex)), index(std::move(o.index)), textures(std::move(o.textures)),
      m_vbo(o.m_vbo), m_ebo(o.m_ebo),
      isSimpleGeometry(o.isSimpleGeometry) {
    o.m_vbo = o.m_ebo = {};
}

Mesh& Mesh::operator=(Mesh&& o) noexcept {
    if (this != &o) {
        releaseMeshBuffers(m_vbo, m_ebo);
        vertex = std::move(o.vertex); index = std::move(o.index); textures = std::move(o.textures);
        m_vbo = o.m_vbo; m_ebo = o.m_ebo;
        isSimpleGeometry = o.isSimpleGeometry;
        o.m_vbo = o.m_ebo = {};
    }
    return *this;
}

void Mesh::drawRHI(Haruka::RHI::Context& ctx) const {
    if (index.empty()) return;
    if (!RHI::valid(m_vbo) || !RHI::valid(m_ebo)) {
        static bool s_warned = false;
        if (!s_warned) {
            HARUKA_LOGW("Mesh", "drawRHI con buffers INVALIDOS (vbo=%u ebo=%u, %zu idx) "
                        "-> el draw se salta", m_vbo.id, m_ebo.id, index.size());
            s_warned = true;
        }
        return;
    }
    ctx.bindVertexBuffer(m_vbo);
    ctx.bindIndexBuffer(m_ebo);
    ctx.drawIndexed(static_cast<uint32_t>(index.size()));
}

void Mesh::setupMesh() {
    RHI::Device* dev = RHI::device();
    m_vbo = dev->createBuffer(RHI::BufferUsage::Vertex, vertex.size() * sizeof(Vertex), &vertex[0]);
    m_ebo = dev->createBuffer(RHI::BufferUsage::Index,  index.size() * sizeof(unsigned int), &index[0]);
}

void Mesh::setupSimpleMesh(const std::vector<glm::vec3>& vertices,
                           const std::vector<glm::vec3>& normals,
                           const std::vector<unsigned int>& indices) {
    vertex.clear();
    vertex.reserve(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        Vertex v{};
        v.Position  = vertices[i];
        v.Normal    = (i < normals.size()) ? normals[i] : glm::vec3(0.0f, 1.0f, 0.0f);
        // UV por PROYECCIÓN DE CAJA (eje dominante de la normal → las otras dos coordenadas).
        // `PrimitiveShapes` no genera UV, y dejarlas a (0,0) hacía que cualquier textura de
        // material sobre un cubo/esfera/plano se resolviera a UN SOLO téxel: el objeto salía de
        // color plano y parecía que el material no se estaba aplicando. No es un desplegado
        // bueno (hay costura en las aristas), pero es continuo dentro de cada cara.
        {
            const glm::vec3& n = v.Normal;
            const glm::vec3 a(std::abs(n.x), std::abs(n.y), std::abs(n.z));
            if (a.x >= a.y && a.x >= a.z)      v.TexCoords = glm::vec2(v.Position.z, v.Position.y);
            else if (a.y >= a.z)               v.TexCoords = glm::vec2(v.Position.x, v.Position.z);
            else                               v.TexCoords = glm::vec2(v.Position.x, v.Position.y);
            v.TexCoords += glm::vec2(0.5f);   // primitivas centradas en el origen → UV a [0,1]
        }
        v.Tangent   = glm::vec3(1.0f, 0.0f, 0.0f);
        v.Bitangent = glm::vec3(0.0f, 1.0f, 0.0f);
        vertex.push_back(v);
    }
    index = indices;
    setupMesh();
}

}} // namespace Haruka::Renderer
