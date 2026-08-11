/**
 * @file shallow_water_renderer.h
 * @brief Renders a ShallowWaterSim heightfield as a dynamic surface mesh
 *        (Fase D). Streams a camera-relative VBO each frame, only emitting
 *        cells that hold water. Reuses the softbody shader (lit surface) tinted
 *        as water; the global ocean keeps its own Gerstner shader.
 */
#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include "tools/math_types.h"
#include "rhi/rhi_types.h"

namespace Haruka { namespace Renderer { class Shader; } } using Haruka::Renderer::Shader;

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
    // Ruta PSO: el pipeline hornea shader + layout + estado (blend alfa, SIN culling — el agua es
    // a dos caras). Ya no hay Shader ni VAO propios.
    Haruka::RHI::PipelineHandle m_pso;
    Haruka::RHI::BufferHandle   m_vboH, m_eboH;   // buffers Stream
    Haruka::RHI::BufferHandle   m_uboH;           // ShallowWaterParams (binding 6)
    std::vector<float>        m_verts;   // pos(3)+normal(3)+alpha+espuma, REUSADO entre frames
    std::vector<unsigned int> m_indices;
    /// Posiciones de la rejilla, reusadas. Antes era un `std::vector` local: una asignación en el
    /// heap de n² elementos por frame, para un tamaño que solo cambia si cambia la rejilla.
    std::vector<glm::vec3>    m_pos;
    bool m_init = false;

    void ensureGL();
};

} // namespace Haruka

