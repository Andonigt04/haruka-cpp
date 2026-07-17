/**
 * @file render_target.h
 * @brief Color render target wrapper (FBO + color texture + depth RBO).
 */
#pragma once
#include <glad/glad.h>
#include "rhi/rhi_types.h"

namespace Haruka { namespace Renderer {

/**
 * @brief Color render target wrapper (FBO + color texture + depth RBO).
 */
class RenderTarget {
public:
    /** @brief Creates target resources with fixed dimensions. */
    RenderTarget(unsigned int width, unsigned int height);
    ~RenderTarget();

    /** @brief Binds FBO for draw/write operations. */
    void bindForWriting();
    /** @brief Restores default framebuffer binding. */
    void unbind();
    /** @brief Binds color texture for shader read at texture unit. */
    void bindForReading(unsigned int textureUnit);

    /** @brief Returns color attachment texture id. */
    unsigned int getColorTexture() const { return colorTexture; }
    /** @brief Returns framebuffer object id. */
    unsigned int getFBO() const { return FBO; }

    /** @brief Handle RHI de la textura de color — la ruta PSO (Context::bindTexture) necesita el
     *  HANDLE, no el id GL. getColorTexture()/getFBO() (ids nativos) son el crutch de la transición
     *  y desaparecerán cuando todos los pases dibujen por el Context. */
    Haruka::RHI::TextureHandle    getColorTextureHandle() const { return m_colorTex; }
    /** @brief Handle RHI del pass (para Context::beginRenderPass). */
    Haruka::RHI::RenderPassHandle getPass() const { return m_pass; }

private:
    void setupFramebuffer();

    // Ruta RHI: el pass posee el FBO/color/depth. FBO y colorTexture cachean los ids GL nativos
    // para que getFBO()/getColorTexture() y los consumidores directos-GL sigan funcionando.
    Haruka::RHI::RenderPassHandle m_pass;
    Haruka::RHI::TextureHandle    m_colorTex;   // handle de la color attachment (ruta PSO)

    unsigned int FBO = 0;          // id GL nativo (cache) o propio (fallback)
    unsigned int colorTexture = 0; // id GL nativo (cache) o propio (fallback)
    unsigned int rboDepth = 0;     // solo usado en la ruta de fallback
    unsigned int width, height;
};

}} // namespace Haruka::Renderer

using Haruka::Renderer::RenderTarget;
namespace Haruka { using Renderer::RenderTarget; }