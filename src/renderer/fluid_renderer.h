/**
 * @file fluid_renderer.h
 * @brief Draws PBF fluid particles as lit point-sprite spheres (Fase C).
 *        Streams a camera-relative position VBO each frame. Self-contained,
 *        called from the game's onRenderWorld hook like the softbody renderer.
 */
#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include "tools/math_types.h"

class Shader;

namespace Haruka {
namespace fluid { class PBFSolver; }

class FluidRenderer {
public:
    FluidRenderer() = default;
    ~FluidRenderer();

    void setSolver(fluid::PBFSolver* s) { m_solver = s; }
    void setRadius(float r) { m_radius = r; }

    /** @brief Uploads particle positions and draws them. Per-frame UBO (binding 0)
     *  must already be bound. viewportH is the framebuffer height in pixels. */
    void render(const Haruka::WorldPos& cameraPos, float viewportH);

private:
    fluid::PBFSolver* m_solver = nullptr;
    std::unique_ptr<Shader> m_shader;
    GLuint m_vao = 0, m_vbo = 0;
    std::vector<float> m_verts;
    float m_radius = 0.3f;
    bool m_init = false;
    void ensureGL();
};

} // namespace Haruka
