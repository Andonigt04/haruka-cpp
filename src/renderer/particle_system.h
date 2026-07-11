#pragma once
/**
 * @file particle_system.h
 * @brief Sistema de partículas de efectos del MOTOR (genérico, reusable): GL_POINTS
 *        radiantes (blend aditivo), núcleo brillante, gravedad + fade. Singleton. El juego
 *        (u otro) llama a emit(); update()/render() se conducen donde convenga.
 */
#include <vector>
#include <memory>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include "rhi/rhi_types.h"

namespace Haruka { namespace Renderer { class Shader; } } using Haruka::Renderer::Shader;

namespace Haruka {

class ParticleSystem {
public:
    static ParticleSystem& get();
    ~ParticleSystem();

    /** @brief Emite n partículas en 'pos' saliendo hacia 'up' + dispersión. color (±var),
     *  speed (m/s), life (s), size (m, pequeño). */
    void emit(const glm::dvec3& pos, int n, const glm::vec3& color,
              float speed, float life, float size, const glm::dvec3& up);
    void update(double dt, const glm::dvec3& up);   // gravedad hacia -up + fade
    void render(const glm::dvec3& camPos);          // GL_POINTS aditivos
    void clear();

private:
    ParticleSystem() = default;
    struct P { glm::dvec3 pos; glm::dvec3 vel; glm::vec3 color; float life; float maxLife; float size; };
    std::vector<P> m_parts;
    std::unique_ptr<Shader> m_shader;
    GLuint m_vao = 0, m_vbo = 0;
    Haruka::RHI::BufferHandle m_vboH;   // VBO Stream (RHI); VAO sigue GL
    void ensureGL();
};

} // namespace Haruka
