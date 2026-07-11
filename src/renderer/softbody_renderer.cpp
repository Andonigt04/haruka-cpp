#include "softbody_renderer.h"
#include "shader.h"
#include "rhi/rhi_device.h"
#include <glm/glm.hpp>

namespace Haruka {

SoftBodyRenderer::~SoftBodyRenderer() { clear(); }

void SoftBodyRenderer::clear() {
    RHI::Device* dev = RHI::device();
    for (auto& e : m_entries) {
        if (dev && RHI::valid(e.hVbo)) { dev->destroy(e.hVbo); dev->destroy(e.hEbo); }
        else { if (e.vbo) glDeleteBuffers(1, &e.vbo); if (e.ebo) glDeleteBuffers(1, &e.ebo); }
        if (e.vao) glDeleteVertexArrays(1, &e.vao);
    }
    m_entries.clear();
}

void SoftBodyRenderer::ensureShader() {
    if (m_shader) return;
    m_shader = std::make_unique<Shader>("shaders/softbody.vert", "shaders/softbody.frag");
}

void SoftBodyRenderer::add(const xpbd::SoftBodyHandle& handle, xpbd::XPBDSolver* solver,
                           const glm::vec3& color) {
    Entry e;
    e.handle = handle;
    e.solver = solver;
    e.color  = color;
    glGenVertexArrays(1, &e.vao);
    glBindVertexArray(e.vao);

    if (RHI::Device* dev = RHI::device()) {
        // EBO estático (índices de render) + VBO Stream (posiciones re-subidas cada frame).
        e.hEbo = dev->createBuffer(RHI::BufferUsage::Index, handle.renderIndices.size() * sizeof(unsigned int),
                                   handle.renderIndices.data());
        e.hVbo = dev->createBuffer(RHI::BufferUsage::Vertex, 0, nullptr, RHI::BufferMemory::Stream);
        e.ebo = dev->nativeBuffer(e.hEbo); e.vbo = dev->nativeBuffer(e.hVbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, e.ebo);
    } else {
        glGenBuffers(1, &e.vbo);
        glGenBuffers(1, &e.ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, e.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     handle.renderIndices.size() * sizeof(unsigned int),
                     handle.renderIndices.data(), GL_STATIC_DRAW);
    }

    glBindBuffer(GL_ARRAY_BUFFER, e.vbo);
    // pos(3) + normal(3)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    e.initialized = true;
    m_entries.push_back(std::move(e));
}

void SoftBodyRenderer::uploadEntry(Entry& e, const Haruka::WorldPos& cameraPos) {
    auto& ps = e.solver->particles;
    const int base = e.handle.firstParticle;
    const int cnt  = e.handle.particleCount;

    // Camera-relative position for each particle (float-safe).
    std::vector<glm::vec3> camPos(cnt);
    for (int i = 0; i < cnt; ++i)
        camPos[i] = glm::vec3(ps.worldPos(base + i) - glm::dvec3(cameraPos));

    // Per-vertex normals from the render triangles (smooth).
    std::vector<glm::vec3> nrm(cnt, glm::vec3(0.0f));
    const auto& idx = e.handle.renderIndices;
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        int ia = (int)idx[t]   - base;
        int ib = (int)idx[t+1] - base;
        int ic = (int)idx[t+2] - base;
        if (ia < 0 || ib < 0 || ic < 0 || ia >= cnt || ib >= cnt || ic >= cnt) continue;
        glm::vec3 fn = glm::cross(camPos[ib] - camPos[ia], camPos[ic] - camPos[ia]);
        nrm[ia] += fn; nrm[ib] += fn; nrm[ic] += fn;
    }

    e.cpuVerts.resize(cnt * 6);
    for (int i = 0; i < cnt; ++i) {
        glm::vec3 n = nrm[i];
        float l = glm::length(n);
        n = (l > 1e-8f) ? n / l : glm::vec3(0, 1, 0);
        e.cpuVerts[i*6+0] = camPos[i].x; e.cpuVerts[i*6+1] = camPos[i].y; e.cpuVerts[i*6+2] = camPos[i].z;
        e.cpuVerts[i*6+3] = n.x;         e.cpuVerts[i*6+4] = n.y;         e.cpuVerts[i*6+5] = n.z;
    }

    if (RHI::Device* dev = RHI::device()) {
        dev->uploadBuffer(e.hVbo, e.cpuVerts.size() * sizeof(float), e.cpuVerts.data());
    } else {
        glBindVertexArray(e.vao);
        glBindBuffer(GL_ARRAY_BUFFER, e.vbo);
        glBufferData(GL_ARRAY_BUFFER, e.cpuVerts.size() * sizeof(float),
                     e.cpuVerts.data(), GL_DYNAMIC_DRAW);
        glBindVertexArray(0);
    }
}

void SoftBodyRenderer::render(const Haruka::WorldPos& cameraPos) {
    if (m_entries.empty()) return;
    ensureShader();
    m_shader->use();
    // Per-frame UBO is already bound at binding 0 by the engine for this frame;
    // the shader's PerFrameData block resolves against it. Do not rebind.

    GLboolean cullWas = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_CULL_FACE); // softbodies (esp. cloth) are double-sided

    const GLint locColor = 15;
    for (auto& e : m_entries) {
        if (!e.initialized || !e.solver) continue;
        uploadEntry(e, cameraPos);
        glUniform3fv(locColor, 1, &e.color[0]);
        glBindVertexArray(e.vao);
        glDrawElements(GL_TRIANGLES, (GLsizei)e.handle.renderIndices.size(),
                       GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(0);
    if (cullWas) glEnable(GL_CULL_FACE);
}

} // namespace Haruka
