#include "renderer/particle_system.h"
#include "renderer/shader.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"
#include "renderer/shader.h"
#include <cstdlib>
#include <cmath>

namespace Haruka {

ParticleSystem& ParticleSystem::get() { static ParticleSystem s; return s; }

ParticleSystem::~ParticleSystem() {
    // Ruta PSO: el pipeline y el VBO los posee el device. Ya no hay VAO propio.
    if (RHI::Device* dev = RHI::device()) {
        if (RHI::valid(m_vboH)) dev->destroy(m_vboH);
        if (RHI::valid(m_pso))  dev->destroy(m_pso);
    }
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

// === MIGRADO A PSO/Context ===
// Antes: Shader externo + VAO propio + glEnable(BLEND/PROGRAM_POINT_SIZE) + glDrawArrays.
// Ahora todo eso vive en el pipeline: blend ADITIVO (partículas radiantes), depth test on pero
// SIN escribir z, y topología Points (el backend GL activa GL_PROGRAM_POINT_SIZE solo).
void ParticleSystem::render(const glm::dvec3& camPos) {
    if (m_parts.empty()) return;
    RHI::Device* dev = RHI::device();
    if (!dev) return;

    if (!RHI::valid(m_pso)) {
        // OJO: createPipeline hace un ifstream CRUDO → hay que enraizar con el base dir de assets.
        const std::string vs = Shader::baseDir() + "shaders/particle.vert";
        const std::string fs = Shader::baseDir() + "shaders/particle.frag";
        RHI::PipelineDesc pd;
        pd.vertexPath          = vs.c_str();
        pd.fragmentPath        = fs.c_str();
        pd.vertexLayout.strides = { (uint32_t)(8 * sizeof(float)) };           // pos(3) color(3) alpha(1) size(1)
        pd.vertexLayout.attributes = {
            { 0, 0,                 RHI::Format::RGB32F },
            { 1, 3 * sizeof(float), RHI::Format::RGB32F },
            { 2, 6 * sizeof(float), RHI::Format::R32F   },
            { 3, 7 * sizeof(float), RHI::Format::R32F   },
        };
        pd.topology     = RHI::PrimitiveTopology::Points;
        pd.depth.test   = true;  pd.depth.write = false;      // no ocluyen entre sí
        pd.blend.enable = true;  pd.blend.mode  = RHI::BlendMode::Additive;
        m_pso  = dev->createPipeline(pd);
        m_vboH = dev->createBuffer(RHI::BufferUsage::Vertex, 0, nullptr, RHI::BufferMemory::Stream);
    }
    if (!RHI::valid(m_pso)) return;   // shader roto: createPipeline ya lo logueó

    std::vector<float> data; data.reserve(m_parts.size() * 8);
    for (const auto& p : m_parts) {
        glm::vec3 rp = glm::vec3(p.pos - camPos);          // camera-relativo
        float a = p.life / p.maxLife;
        data.insert(data.end(), { rp.x, rp.y, rp.z, p.color.r, p.color.g, p.color.b, a, p.size });
    }
    dev->uploadBuffer(m_vboH, data.size() * sizeof(float), data.data());  // Stream: orphan + subida

    RHI::Context* ctx = dev->beginFrame();
    ctx->bindPipeline(m_pso);          // programa + blend aditivo + depth/point-size
    ctx->bindVertexBuffer(m_vboH);
    ctx->draw((uint32_t)m_parts.size());

    // Transición: el resto del frame sigue en GL directo y espera este estado.
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}

} // namespace Haruka
