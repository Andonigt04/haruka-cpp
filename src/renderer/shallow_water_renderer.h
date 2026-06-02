/**
 * @file shallow_water_renderer.h
 * @brief Renders a ShallowWaterSim heightfield as a dynamic surface mesh
 *        (Fase D). Streams a camera-relative VBO each frame, only emitting
 *        cells that hold water. Reuses the softbody shader (lit surface) tinted
 *        as water; the global ocean keeps its own Gerstner shader.
 */
#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include "tools/math_types.h"

class Shader;

namespace Haruka {
namespace fluid { class ShallowWaterSim; }

class ShallowWaterRenderer {
public:
    ShallowWaterRenderer() = default;
    ~ShallowWaterRenderer();

    void setSim(fluid::ShallowWaterSim* sim) { m_sim = sim; }

    /** @brief Rebuilds the surface mesh from current water depths and draws it.
     *  Per-frame UBO (binding 0) must already be bound by the caller. */
    void render(const Haruka::WorldPos& cameraPos);

private:
    fluid::ShallowWaterSim* m_sim = nullptr;
    std::unique_ptr<Shader> m_shader;
    GLuint m_vao = 0, m_vbo = 0, m_ebo = 0;
    std::vector<float>        m_verts;   // pos(3)+normal(3)
    std::vector<unsigned int> m_indices;
    bool m_init = false;

    void ensureGL();
};

} // namespace Haruka
