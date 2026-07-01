#include "shallow_water_renderer.h"
#include "shader.h"
#include "physics/fluid/shallow_water.h"
#include <glm/glm.hpp>

namespace Haruka {

ShallowWaterRenderer::~ShallowWaterRenderer() {
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_ebo) glDeleteBuffers(1, &m_ebo);
}

void ShallowWaterRenderer::ensureGL() {
    if (m_init) return;
    m_shader = std::make_unique<Shader>("shaders/softbody.vert", "shaders/softbody.frag");
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    // Vertex: pos(3) + normal(3) + alpha(1) = 7 floats. Alpha (loc 2) drives the
    // depth-based edge fade in softbody.frag (u_alphaMode), softening the grid.
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 7*sizeof(float), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 7*sizeof(float), (void*)(6*sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    m_init = true;
}

void ShallowWaterRenderer::render(const Haruka::WorldPos& cameraPos) {
    if (!m_sim) return;
    ensureGL();

    const int n = m_sim->size();
    if (n < 2) return;

    // Surface position of cell (i,j): anchor-plane world pos lifted by
    // (terrain + water) along the up axis. Camera-relative for float safety.
    const glm::dvec3 up = m_sim->up();
    auto surfPos = [&](int i, int j) -> glm::vec3 {
        glm::dvec3 base = m_sim->worldPosAt(i, j);
        double lift = double(m_sim->terrainAt(i,j)) + double(m_sim->waterAt(i,j));
        glm::dvec3 wp = base + up * lift;
        return glm::vec3(wp - glm::dvec3(cameraPos));
    };

    // Build vertex grid (only used cells matter, but a full grid keeps indexing
    // simple; cells without water collapse to terrain and are skipped in tris).
    m_verts.clear();
    m_verts.resize(size_t(n) * n * 7, 0.0f);
    std::vector<glm::vec3> pos(size_t(n) * n);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
            pos[j*n+i] = surfPos(i, j);

    // Smooth normals from neighbours.
    auto P = [&](int i,int j){ i=glm::clamp(i,0,n-1); j=glm::clamp(j,0,n-1); return pos[j*n+i]; };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            glm::vec3 dx = P(i+1,j) - P(i-1,j);
            glm::vec3 dz = P(i,j+1) - P(i,j-1);
            glm::vec3 nrm = glm::cross(dz, dx);
            float l = glm::length(nrm);
            nrm = (l > 1e-8f) ? nrm/l : glm::vec3(up);
            // Opacity fades with water depth so thin boundary water (the cells that
            // make the river look "square") dissolves into a smooth, depth-following
            // waterline instead of a hard grid edge. Deeper water reads as solid.
            float depth = m_sim->waterAt(i, j);
            float alpha = glm::smoothstep(0.02f, 0.30f, depth) * 0.85f;
            int b = (j*n+i)*7;
            m_verts[b+0]=pos[j*n+i].x; m_verts[b+1]=pos[j*n+i].y; m_verts[b+2]=pos[j*n+i].z;
            m_verts[b+3]=nrm.x; m_verts[b+4]=nrm.y; m_verts[b+5]=nrm.z;
            m_verts[b+6]=alpha;
        }

    // Triangles only for quads where at least one corner holds water POR ENCIMA del mar.
    // F5.3: las celdas a/bajo el nivel del mar las dibujan los chunks de océano (Gerstner) →
    // aquí se saltan para que no haya DOBLE lámina en la costa. Sin océano acoplado
    // (seaLevel=-1e9) se dibuja todo como antes.
    const float sea    = m_sim->seaLevelAlongUp();
    const float seaEps = 0.15f; // margen sobre el mar (las celdas fijadas al mar quedan ~en seaLevel)
    auto inlandWet = [&](int i, int j) {
        return m_sim->hasWaterAt(i,j) && m_sim->surfaceAt(i,j) > sea + seaEps;
    };
    m_indices.clear();
    for (int j = 0; j + 1 < n; ++j)
        for (int i = 0; i + 1 < n; ++i) {
            bool wet = inlandWet(i,j) || inlandWet(i+1,j)
                    || inlandWet(i,j+1) || inlandWet(i+1,j+1);
            if (!wet) continue;
            unsigned int a=j*n+i, b=j*n+i+1, c=(j+1)*n+i, d=(j+1)*n+i+1;
            m_indices.insert(m_indices.end(), {a,c,b, b,c,d});
        }
    if (m_indices.empty()) return;

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, m_verts.size()*sizeof(float), m_verts.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indices.size()*sizeof(unsigned int), m_indices.data(), GL_DYNAMIC_DRAW);

    m_shader->use();
    const GLint locColor = 15;
    glm::vec3 waterCol(0.10f, 0.35f, 0.55f);
    glUniform3fv(locColor, 1, &waterCol[0]);
    glUniform1f(16, 1.0f); // u_alphaMode: use the per-vertex depth alpha

    GLboolean cullWas = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glDrawElements(GL_TRIANGLES, (GLsizei)m_indices.size(), GL_UNSIGNED_INT, nullptr);

    glBindVertexArray(0);
    if (cullWas) glEnable(GL_CULL_FACE);
}

} // namespace Haruka
