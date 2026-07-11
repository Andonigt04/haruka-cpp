#include "renderer/particle_system.h"
#include "renderer/shader.h"
#include "rhi/rhi_device.h"
#include <cstdlib>
#include <cmath>

namespace Haruka {

ParticleSystem& ParticleSystem::get() { static ParticleSystem s; return s; }

ParticleSystem::~ParticleSystem() {
    if (RHI::valid(m_vboH)) { if (RHI::Device* dev = RHI::device()) dev->destroy(m_vboH); }
    else if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
}

void ParticleSystem::clear() { m_parts.clear(); }

static float frand() { return (float)std::rand() / (float)RAND_MAX; }

void ParticleSystem::emit(const glm::dvec3& pos, int n, const glm::vec3& color,
                          float speed, float life, float size, const glm::dvec3& up) {
    glm::dvec3 u = (glm::length(up) > 1e-6) ? glm::normalize(up) : glm::dvec3(0, 1, 0);
    glm::dvec3 ref = (std::abs(u.y) < 0.99) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    glm::dvec3 t = glm::normalize(glm::cross(ref, u));
    glm::dvec3 b = glm::cross(u, t);
    for (int i = 0; i < n; ++i) {
        glm::dvec3 dir = u * (0.6 + 0.8 * frand())                 // sobre todo hacia arriba
                       + t * (double)(frand() - 0.5f) * 1.6
                       + b * (double)(frand() - 0.5f) * 1.6;
        dir = glm::normalize(dir);
        P p;
        p.pos     = pos + u * 0.08;
        p.vel     = dir * (double)(speed * (0.5f + frand()));
        p.color   = color * (0.9f + 0.5f * frand());               // ligera variación
        p.maxLife = p.life = life * (0.7f + 0.6f * frand());
        p.size    = size * (0.6f + 0.7f * frand());                // pequeñas
        m_parts.push_back(p);
    }
}

void ParticleSystem::update(double dt, const glm::dvec3& up) {
    glm::dvec3 g = -((glm::length(up) > 1e-6) ? glm::normalize(up) : glm::dvec3(0, 1, 0)) * 13.0;
    for (auto it = m_parts.begin(); it != m_parts.end(); ) {
        it->vel += g * dt;
        it->pos += it->vel * dt;
        it->life -= (float)dt;
        if (it->life <= 0.0f) it = m_parts.erase(it); else ++it;
    }
}

void ParticleSystem::ensureGL() {
    if (m_vao) return;
    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);
    if (RHI::Device* dev = RHI::device()) {
        m_vboH = dev->createBuffer(RHI::BufferUsage::Vertex, 0, nullptr, RHI::BufferMemory::Stream);
        m_vbo = dev->nativeBuffer(m_vboH);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    } else {
        glGenBuffers(1, &m_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    }
    const GLsizei stride = 8 * sizeof(float);   // pos(3) color(3) alpha(1) size(1)
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void*)(7 * sizeof(float)));
    glBindVertexArray(0);
}

void ParticleSystem::render(const glm::dvec3& camPos) {
    if (m_parts.empty()) return;
    if (!m_shader) m_shader = std::make_unique<Shader>("shaders/particle.vert", "shaders/particle.frag");
    ensureGL();

    std::vector<float> data; data.reserve(m_parts.size() * 8);
    for (const auto& p : m_parts) {
        glm::vec3 rp = glm::vec3(p.pos - camPos);          // camera-relativo
        float a = p.life / p.maxLife;
        data.insert(data.end(), { rp.x, rp.y, rp.z, p.color.r, p.color.g, p.color.b, a, p.size });
    }

    glBindVertexArray(m_vao);
    if (RHI::Device* dev = RHI::device()) dev->uploadBuffer(m_vboH, data.size() * sizeof(float), data.data());
    else { glBindBuffer(GL_ARRAY_BUFFER, m_vbo); glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(), GL_DYNAMIC_DRAW); }
    m_shader->use();
    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);  // ADITIVO → radiante
    glEnable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
    glDrawArrays(GL_POINTS, 0, (GLsizei)m_parts.size());
    glDepthMask(GL_TRUE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}

} // namespace Haruka
