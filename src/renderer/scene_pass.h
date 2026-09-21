#pragma once
/**
 * @file renderer/scene_pass.h
 * @brief EL PASE DE ESCENA como modulo: los OBJETOS de la escena (modelos, componentes de malla,
 *        primitivas; cada uno con su UBO por objeto y su material) y las PIEZAS DE CONSTRUCCION
 *        instanciadas (agrupadas por malla + material, un draw por grupo). Abre el render pass de
 *        escena sin clear (el cielo ya esta pintado) y lo deja abierto para que los props dibujen
 *        detras; el cierre lo hace quien llama. Vivia en `Application::passSceneObjects` (~290
 *        lineas) y el arranque en `passSceneSetup`.
 */
#include <memory>
#include <vector>

#include "core/scene/scene_render_policy.h"   // RenderCommand
#include "renderer/gpu_instancing.h"
#include "rhi/rhi_types.h"
#include "rhi/rhi_resources.h"

namespace Haruka { namespace Core { class Camera; } class PlanetarySystem; namespace RHI { class Context; } }

namespace Haruka::Renderer {

class ScenePass {
public:
    struct Frame {
        Core::Camera*         camera  = nullptr;
        PlanetarySystem*      planets = nullptr;    ///< la vertical del planeta para los personajes
        RHI::BufferHandle     perFrameUBO{};        ///< binding 0 (view/proj + luces)
        RHI::RenderPassHandle sceneTargetPass{};    ///< invalido = backbuffer
        uint32_t width = 1, height = 1;
    };
    /// TOTAL = lo que el frame podia dibujar antes de culling; RENDERED = lo que entro.
    struct DrawStats {
        int renderedDrawCalls = 0, renderedVertices = 0, renderedTriangles = 0;
        int totalDrawCalls = 0,    totalVertices = 0,    totalTriangles = 0;
    };

    ~ScenePass();
    void shutdown();
    void beginFrame() { if (m_instancing) m_instancing->beginFrame(); }

    /// Crea (o recrea, si cambia el look) el pipeline, abre el pass SIN clear y ata los UBO.
    /// `useFinalLook` = final.frag (HDR/bloom/SSAO/IBL/sombras) frente a preview.frag.
    void begin(RHI::Context* ctx, const Frame& f, bool useFinalLook);
    /// Los objetos de la cola + las piezas de construccion instanciadas. Deja atado `pso()`.
    void drawObjects(RHI::Context* ctx, const Frame& f, const std::vector<Haruka::RenderCommand>& queue, DrawStats& st);

    RHI::PipelineHandle pso() const { return m_scenePSO; }        ///< para restaurarlo tras otro pase
    RHI::BufferHandle   perObjectUBO();                           ///< binding 1 (lo crea si falta)
    size_t hostBytes() const { return m_instancing ? m_instancing->hostBytes() : 0; }

private:
    RHI::PipelineHandle m_scenePSO{};
    bool                m_scenePSOFinalLook = false;
    RHI::BufferHandle   m_perObjectUBO{};
    /// Piezas de construccion: mismo Vertex + stream de instancia; un draw por (malla, material).
    RHI::PipelineHandle m_constInstPSO{};
    RHI::BufferHandle   m_constParamsUBO{};   // ConstParams (binding 6)
    std::unique_ptr<GPUInstancing> m_instancing;
};

} // namespace Haruka::Renderer
