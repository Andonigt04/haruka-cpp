/**
 * @file softbody_renderer.h
 * @brief Renders XPBD softbodies by streaming particle positions into a dynamic
 *        VBO each frame (Fase B). Self-contained (own pipeline/VBO/shader), drawn from
 *        the game's onRenderWorld hook — decoupled like the water/terrain renderers.
 *
 * Positions are camera-relative (particle.origin - cameraPos + localPos), so the
 * vertex shader only needs view-rotation + projection, matching the planet/water
 * convention and staying float-safe at planetary distances.
 */
#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include "rhi/rhi_types.h"
#include "physics/xpbd/softbody.h"
#include "tools/math_types.h"

namespace Haruka { namespace Renderer { class Shader; } } using Haruka::Renderer::Shader;

namespace Haruka {

class SoftBodyRenderer {
public:
    SoftBodyRenderer() = default;
    ~SoftBodyRenderer();

    void add(const xpbd::SoftBodyHandle& handle, xpbd::XPBDSolver* solver,
             const glm::vec3& color = glm::vec3(0.8f, 0.2f, 0.25f));

    void render(const Haruka::WorldPos& cameraPos);

    void clear();

private:
    struct Entry {
        xpbd::SoftBodyHandle handle;
        xpbd::XPBDSolver*    solver = nullptr;
        glm::vec3            color{0.8f};
        Haruka::RHI::BufferHandle hVbo, hEbo;
        std::vector<float> cpuVerts;
        bool initialized = false;
    };
    std::vector<Entry> m_entries;
    Haruka::RHI::PipelineHandle m_pso;
    Haruka::RHI::BufferHandle   m_paramsUBO; // SoftbodyParams (binding 6)

    void ensurePSO();
    void uploadEntry(Entry& e, const Haruka::WorldPos& cameraPos);
};

} // namespace Haruka

