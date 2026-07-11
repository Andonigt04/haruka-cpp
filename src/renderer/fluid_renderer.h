/**
 * @file fluid_renderer.h
 * @brief Draws PBF fluid particles. Two modes:
 *   - Screen-space surface (default): depth pass → bilateral smooth → surface
 *     composite, so the particles read as one liquid surface, not spheres.
 *   - Sphere sprites (fallback, s_surfaceMode=false): the original lit point
 *     sprites, kept for comparison / if the surface look needs disabling.
 * Self-contained; called from the game's onRenderWorld hook.
 */
#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include "tools/math_types.h"
#include "rhi/rhi_types.h"

namespace Haruka { namespace Renderer { class Shader; } } using Haruka::Renderer::Shader;

namespace Haruka {
namespace fluid { class PBFSolver; }

class FluidRenderer {
public:
    FluidRenderer() = default;
    ~FluidRenderer();

    void setSolver(fluid::PBFSolver* s) { m_solver = s; }
    void setRadius(float r) { m_radius = r; }

    /** @brief Uploads particle positions and draws the fluid. Per-frame UBO
     *  (binding 0) must already be bound. vpW/vpH = framebuffer size in pixels. */
    void render(const Haruka::WorldPos& cameraPos, int vpW, int vpH);

    // Switch between the screen-space surface (true) and the sphere fallback.
    static inline bool s_surfaceMode = true;

private:
    fluid::PBFSolver* m_solver = nullptr;
    std::vector<float> m_verts;
    float m_radius = 0.3f;
    bool m_init = false;

    // Particle point cloud.
    GLuint m_vao = 0, m_vbo = 0;
    // Handles RHI (buffers + render targets); los ids GL de abajo son cache de los nativos.
    Haruka::RHI::BufferHandle m_particleBuf, m_quadBuf;
    Haruka::RHI::RenderPassHandle m_depthPass, m_smoothPass[2], m_sceneCopyPass;
    std::unique_ptr<Shader> m_sphereShader;   // fallback lit spheres

    // Screen-space surface pipeline.
    std::unique_ptr<Shader> m_depthShader;    // particle → eye depth
    std::unique_ptr<Shader> m_blurShader;     // separable bilateral
    std::unique_ptr<Shader> m_surfaceShader;  // composite
    GLuint m_quadVAO = 0, m_quadVBO = 0;
    GLuint m_depthFBO = 0, m_depthTex = 0, m_depthRB = 0;
    GLuint m_smoothFBO[2] = {0, 0}, m_smoothTex[2] = {0, 0};
    GLuint m_sceneCopyFBO = 0, m_sceneCopyTex = 0; // copy of scene colour (refraction)
    int    m_fbW = 0, m_fbH = 0;

    void ensureGL();
    void ensureTargets(int w, int h);
    void uploadParticles(const Haruka::WorldPos& cameraPos, int n);
    void renderSpheres(int n, float vpH);
    void renderSurface(int n, int vpW, int vpH);
};

} // namespace Haruka

