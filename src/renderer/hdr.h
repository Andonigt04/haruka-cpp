/**
 * @file hdr.h
 * @brief HDR framebuffer with color and bright-pass attachments for tone mapping.
 */
#pragma once

#include <glad/glad.h>
#include "rhi/rhi_types.h"

namespace Haruka { namespace Renderer {

/**
 * @brief HDR framebuffer wrapper with color and bright attachments.
 */
class HDR
{
public:
    /** @brief Creates HDR framebuffer at the requested resolution. */
    HDR(unsigned int width, unsigned int height);
    ~HDR();

    /** @brief Binds HDR FBO for scene rendering. */
    void bindForWriting();
    /** @brief Restores default framebuffer binding. */
    void unbind();
    /** @brief Binds HDR attachments for post-processing reads. */
    void bindForReading(unsigned int textureUnit, int index);

    /** @brief Returns HDR color texture id. */
    unsigned int getColorTexture() { return colorTexture; }
    /** @brief Returns HDR bright-pass texture id. */
    unsigned int getBrightTexture() { return brightTexture; }
    /** @brief Returns HDR framebuffer id. */
    unsigned int getFBO() { return hdrFBO; }

    /** @brief Handle RHI de la textura de color — la ruta PSO (Context::bindTexture) necesita el
     *  HANDLE, no el id GL. Los get*Texture()/getFBO() (ids nativos) son el crutch de la transición
     *  y desaparecerán cuando todos los pases dibujen por el Context. */
    Haruka::RHI::TextureHandle    getColorTextureHandle() const { return m_colorTex; }
    /** @brief Handle RHI del pass (para Context::beginRenderPass). */
    Haruka::RHI::RenderPassHandle getPass() const { return m_pass; }
private:
    /** @brief Allocates framebuffer and attachments. */
    void setupFramebuffer();
    Haruka::RHI::RenderPassHandle m_pass;   // ruta RHI (los ids GL cacheados abajo)
    Haruka::RHI::TextureHandle    m_colorTex;  // handle de la color attachment (ruta PSO)
    unsigned int hdrFBO;
    unsigned int colorTexture;
    unsigned int brightTexture;
    unsigned int rboDepth;
    unsigned int width, height;
};

}} // namespace Haruka::Renderer

using Haruka::Renderer::HDR;
namespace Haruka { using Renderer::HDR; }