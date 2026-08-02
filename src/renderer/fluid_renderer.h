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

#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include "tools/math_types.h"
#include "rhi/rhi_types.h"

namespace Haruka::RHI { class Context; }

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
    void render(const Haruka::WorldPos& cameraPos, int vpW, int vpH,
                Haruka::RHI::RenderPassHandle scenePass = {});

    // Switch between the screen-space surface (true) and the sphere fallback.
    static inline bool s_surfaceMode = true;

private:
    fluid::PBFSolver* m_solver = nullptr;
    std::vector<float> m_verts;
    float m_radius = 0.3f;
    bool m_init = false;

    // Handles RHI (buffers + render targets). Los FBO/textura GL de abajo se conservan porque
    // los BLITS (copia de color/depth de la escena) siguen en GL: el RHI no tiene comando de blit.
    Haruka::RHI::BufferHandle m_particleBuf, m_quadBuf;
    Haruka::RHI::RenderPassHandle m_depthPass, m_smoothPass[2], m_sceneCopyPass;

    // Los 4 PSO del fluido (antes: 4 Shader + VAOs + glUniform + glEnable sueltos).
    //  - spheres: puntos, depth on           (fallback de esferas)
    //  - depth:   puntos → profundidad de ojo
    //  - blur:    quad, bilateral separable
    //  - surface: quad, composite final
    Haruka::RHI::PipelineHandle m_psoSpheres, m_psoDepth, m_psoBlur, m_psoSurface;
    // UBO del pase (binding 8), compartido por los 4 programas: el bloque es idéntico en todos.
    struct FluidParamsUBO {
        glm::vec2 blurDir{0.0f};
        glm::vec2 texel{0.0f};
        float depthFalloff = 0.5f;
        float refractScale = 0.03f;
        float radius       = 0.3f;
        float viewportH    = 1.0f;
    };
    static_assert(sizeof(FluidParamsUBO) == 32, "std140: 2×vec2 + 4 floats");
    FluidParamsUBO            m_params{};
    Haruka::RHI::BufferHandle m_uboParams;

    int    m_fbW = 0, m_fbH = 0;

    void ensureGL();
    /** @brief Sube m_params al UBO y lo ata (los 4 pases lo comparten). */
    void bindParams(Haruka::RHI::Context* ctx);
    void ensureTargets(int w, int h);
    void uploadParticles(const Haruka::WorldPos& cameraPos, int n);
    void renderSpheres(int n, float vpH);
    void renderSurface(int n, int vpW, int vpH);
};

} // namespace Haruka

