#include "render_target.h"
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"
#include <iostream>

namespace Haruka { namespace Renderer {

RenderTarget::RenderTarget(unsigned int width, unsigned int height)
    : width(width), height(height) {
    setupFramebuffer();
}

void RenderTarget::setupFramebuffer() {
    // El device crea FBO + color (HDR RGBA16F) + depth (24) y devolvemos los ids nativos.
    RHI::Device* dev = RHI::device();
    RHI::RenderTargetDesc desc;
    desc.width       = width;
    desc.height      = height;
    desc.colorFormats = { RHI::Format::RGBA16F };
    desc.hasDepth    = true;
    desc.depthFormat = RHI::Format::D32F;   // REVERSED-Z: la precisión solo se aprovecha con depth FLOAT
    m_pass       = dev->createRenderTarget(desc);
    FBO          = dev->nativeFramebuffer(m_pass);
    m_colorTex   = dev->getColorTexture(m_pass);      // handle (ruta PSO/Context)
    colorTexture = dev->nativeTexture(m_colorTex);    // id GL (crutch de la transición)

    // Inicializa a negro transparente: RGBA16F sin inicializar puede traer NaN/Inf en VRAM
    // (AMD) y provocar fallos de GPU al muestrearlo.
    glBindFramebuffer(GL_FRAMEBUFFER, FBO);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RenderTarget::bindForWriting() {
    // Bind directo por id nativo (venga de RHI o fallback). El bind se migrará al Context en F3.
    glBindFramebuffer(GL_FRAMEBUFFER, FBO);
    glViewport(0, 0, width, height);
}

void RenderTarget::unbind() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RenderTarget::bindForReading(unsigned int textureUnit) {
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_2D, colorTexture);
}

RenderTarget::~RenderTarget() {
    if (RHI::valid(m_pass))
        if (RHI::Device* dev = RHI::device()) dev->destroy(m_pass);   // libera FBO+color+depth del RHI
}

}} // namespace Haruka::Renderer
