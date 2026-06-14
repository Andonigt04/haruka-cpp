#include "renderer/floating_island_renderer.h"

namespace Haruka {

FloatingIslandRenderer::~FloatingIslandRenderer() { cleanup(); }

void FloatingIslandRenderer::cleanup() {
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_nbo) glDeleteBuffers(1, &m_nbo);
    if (m_ebo) glDeleteBuffers(1, &m_ebo);
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

    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(glm::vec3), verts.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
    glEnableVertexAttribArray(0);

    glGenBuffers(1, &m_nbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_nbo);
    glBufferData(GL_ARRAY_BUFFER, norms.size() * sizeof(glm::vec3), norms.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
    glEnableVertexAttribArray(1);

    glGenBuffers(1, &m_ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(unsigned int), idx.data(), GL_STATIC_DRAW);

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
