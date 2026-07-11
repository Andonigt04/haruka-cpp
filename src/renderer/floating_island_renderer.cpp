#include "renderer/floating_island_renderer.h"
#include "rhi/rhi_device.h"

namespace Haruka {

FloatingIslandRenderer::~FloatingIslandRenderer() { cleanup(); }

void FloatingIslandRenderer::cleanup() {
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (RHI::valid(m_vboH)) {
        if (RHI::Device* dev = RHI::device()) { dev->destroy(m_vboH); dev->destroy(m_nboH); dev->destroy(m_eboH); }
        m_vboH = m_nboH = m_eboH = {};
    } else {
        if (m_vbo) glDeleteBuffers(1, &m_vbo);
        if (m_nbo) glDeleteBuffers(1, &m_nbo);
        if (m_ebo) glDeleteBuffers(1, &m_ebo);
    }
    m_vao = m_vbo = m_nbo = m_ebo = 0;
    m_indexCount = 0;
}

void FloatingIslandRenderer::setIslands(std::vector<FloatingIsland> islands) {
    m_pending  = std::move(islands);
    m_uploaded = false;
}

void FloatingIslandRenderer::uploadPending() {
    cleanup();
    m_islandCount = (int)m_pending.size();
    if (m_pending.empty()) { m_uploaded = true; return; }

    // Fusiona todas las islas en arrays únicos. Vértices relativos a un origen del
    // casquete → precisión float segura.
    m_origin = m_pending[0].worldCenter;
    std::vector<glm::vec3> verts, norms;
    std::vector<unsigned int> idx;
    for (const auto& isl : m_pending) {
        const auto& mesh = isl.mesh;
        if (mesh.vertices.empty() || mesh.indices.empty()) continue;
        glm::vec3 base = glm::vec3(isl.worldCenter - m_origin);
        unsigned int off = (unsigned int)verts.size();
        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            verts.push_back(base + mesh.vertices[i]);
            norms.push_back(mesh.normals[i]);
        }
        for (unsigned int ind : mesh.indices) idx.push_back(off + ind);
    }
    m_indexCount = (uint32_t)idx.size();
    if (m_indexCount == 0) { m_uploaded = true; m_pending.clear(); return; }

    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);

    RHI::Device* dev = RHI::device();
    auto mkStatic = [&](Haruka::RHI::BufferHandle& h, GLuint& glId, RHI::BufferUsage usage, const void* d, size_t bytes) {
        GLenum tgt = (usage == RHI::BufferUsage::Index) ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
        if (dev) { h = dev->createBuffer(usage, bytes, d); glId = dev->nativeBuffer(h); glBindBuffer(tgt, glId); }
        else     { glGenBuffers(1, &glId); glBindBuffer(tgt, glId); glBufferData(tgt, bytes, d, GL_STATIC_DRAW); }
    };

    mkStatic(m_vboH, m_vbo, RHI::BufferUsage::Vertex, verts.data(), verts.size() * sizeof(glm::vec3));
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
    glEnableVertexAttribArray(0);

    mkStatic(m_nboH, m_nbo, RHI::BufferUsage::Vertex, norms.data(), norms.size() * sizeof(glm::vec3));
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
    glEnableVertexAttribArray(1);

    mkStatic(m_eboH, m_ebo, RHI::BufferUsage::Index, idx.data(), idx.size() * sizeof(unsigned int));

    glBindVertexArray(0);
    m_uploaded = true;
    m_pending.clear();
}

void FloatingIslandRenderer::render(const Haruka::WorldPos& cameraPos) {
    if (!m_uploaded) uploadPending();
    if (m_indexCount == 0) return;

    const GLint locOffset = 10; // u_chunkOffset (misma convención que terreno/props)
    glm::vec3 offset = glm::vec3(m_origin - glm::dvec3(cameraPos));
    glUniform3fv(locOffset, 1, &offset[0]);
    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

} // namespace Haruka
