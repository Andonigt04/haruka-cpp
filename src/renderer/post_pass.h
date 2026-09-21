#pragma once
/**
 * @file renderer/post_pass.h
 * @brief EL POST-PROCESO como modulo: el target de escena a escala de render, el bloom (bright-pass
 *        + gaussiana separable a media resolucion) y el composite a pantalla (FXAA + bloom +
 *        upscale). Vivia en `Application` (renderBloom, setupQuad, el bloque del composite y
 *        `_postScene`/`m_bloom*`/`m_present*`); Andoni (21-09): "los modulos estan en application_*".
 *
 * Dos ganchos por frame:
 *   · `begin(width, height, editorTarget)` ANTES del frame: decide si el post esta activo (escala
 *     ≠ 1, FXAA o bloom, y no en el editor) y (re)crea el target de escena. Si esta activo, la
 *     escena se dibuja en `scenePass()` a `renderW()`×`renderH()`.
 *   · `composite(width, height)` al final: vuelca el target a la pantalla con los efectos.
 */
#include <memory>
#include <vector>

#include "rhi/rhi_types.h"
#include "rhi/rhi_resources.h"

namespace Haruka::Renderer {

class HDR;

class PostPass {
public:
    ~PostPass();
    void shutdown();   ///< suelta la GPU (el device sigue vivo)

    /// Decide y prepara el frame. `editorTarget` = el IDE dibuja a su propio target (sin post).
    /// Devuelve `active()`.
    bool begin(int windowW, int windowH, bool editorTarget);
    bool active() const { return m_active; }
    int  renderW() const { return m_active ? m_w : 0; }
    int  renderH() const { return m_active ? m_h : 0; }
    /// Pase del target de escena (invalido si el post no esta activo: la escena va al backbuffer).
    RHI::RenderPassHandle scenePass() const;

    /// Al final del frame: FXAA + bloom + upscale al backbuffer. No hace nada si no esta activo.
    void composite(int windowW, int windowH);

private:
    void setupQuad();
    RHI::TextureHandle renderBloom(RHI::TextureHandle srcColorTex);

    std::unique_ptr<HDR> m_scene;        ///< la escena a escala de render (RGBA16F + depth)
    int  m_w = 0, m_h = 0;
    bool m_active = false;
    bool m_wantFXAA = false, m_wantBloom = false;

    RHI::BufferHandle     m_quadBuf{};
    /// BLOOM por PSO/Context: dos targets ping-pong a media resolucion y un UBO POR DRAW (en Vulkan
    /// los comandos se graban y se ejecutan despues; un UBO compartido leeria el ultimo valor).
    RHI::RenderPassHandle m_bloomPass[2] = {};
    RHI::TextureHandle    m_bloomTexH[2] = {};
    RHI::PipelineHandle   m_bloomExtractPSO{}, m_bloomBlurPSO{};
    std::vector<RHI::BufferHandle> m_bloomUBOs;
    int m_bloomW = 0, m_bloomH = 0;
    RHI::PipelineHandle   m_presentPSO{};
    RHI::BufferHandle     m_presentUBO{};
};

} // namespace Haruka::Renderer
