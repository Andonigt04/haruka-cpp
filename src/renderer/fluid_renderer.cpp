#include "fluid_renderer.h"
#include "shader.h"
#include "physics/fluid/pbf_solver.h"

namespace Haruka {

FluidRenderer::~FluidRenderer() {
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
}

void FluidRenderer::ensureGL() {
    if (m_init) return;
    m_shader = std::make_unique<Shader>("shaders/fluid_particle.vert",
                                        "shaders/fluid_particle.frag");
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
    m_init = true;
}

void FluidRenderer::render(const Haruka::WorldPos& cameraPos, float viewportH) {
    if (!m_solver || m_solver->count() == 0) return;
    ensureGL();

    const int n = m_solver->count();
    m_verts.resize(size_t(n) * 3);
    for (int i = 0; i < n; ++i) {
        glm::vec3 cp = glm::vec3(m_solver->worldPos(i) - glm::dvec3(cameraPos));
        m_verts[i*3+0] = cp.x; m_verts[i*3+1] = cp.y; m_verts[i*3+2] = cp.z;
    }

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, m_verts.size()*sizeof(float), m_verts.data(), GL_DYNAMIC_DRAW);

    m_shader->use();
    glUniform1f(12, m_radius);
    glUniform1f(13, viewportH);

    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_DEPTH_TEST);
    glDrawArrays(GL_POINTS, 0, n);

    glBindVertexArray(0);
}

} // namespace Haruka
