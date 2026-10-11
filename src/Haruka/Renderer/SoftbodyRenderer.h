/**
 * @file SoftbodyRenderer.h
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
#include "Haruka/RHI/RHITypes.h"
#include "Haruka/Physics/XPBD/Softbody.h"
#include "Haruka/Tools/MathTypes.h"

namespace Haruka { namespace Renderer { class Shader; } }

namespace Haruka::Renderer {

class SoftBodyRenderer {
public:
    SoftBodyRenderer() = default;
    ~SoftBodyRenderer();

    /** @brief Registra un cuerpo blando para dibujarlo. Devuelve un id para `remove`. */
    int  add(const Physics::XPBD::SoftBodyHandle& handle, Physics::XPBD::XPBDSolver* solver,
             const glm::vec3& color = glm::vec3(0.8f, 0.2f, 0.25f));
    /** @brief Deja de dibujarlo y libera sus buffers (lo que dibuja una criatura muere con ella). */
    void remove(int id);

    void render(const Tools::WorldPos& cameraPos);

    void clear();

private:
    struct Entry {
        Physics::XPBD::SoftBodyHandle handle;
        Physics::XPBD::XPBDSolver*    solver = nullptr;
        glm::vec3            color{0.8f};
        Haruka::RHI::BufferHandle hVbo, hEbo;
        /// ⚠️ UN UBO DE COLOR POR ENTRADA. Era uno para todas, reescrito entre dibujos: en GL cada
        /// draw lee lo último escrito ANTES de él, pero en Vulkan el búfer se lee al EJECUTAR el
        /// frame, así que todas salían del color de la última entrada.
        Haruka::RHI::BufferHandle hParams;
        int id = 0;
        std::vector<float> cpuVerts;
        bool initialized = false;
    };
    std::vector<Entry> m_entries;
    Haruka::RHI::PipelineHandle m_pso;
    Haruka::RHI::BufferHandle   m_paramsUBO; // (ya no se usa para dibujar: ver Entry::hParams)
    int m_nextId = 1;

    void ensurePSO();
    void uploadEntry(Entry& e, const Tools::WorldPos& cameraPos);
};

} // namespace Haruka::Renderer

