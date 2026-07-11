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
    // Ruta RHI: el device crea FBO + color (HDR RGBA16F) + depth (24) y devolvemos los ids nativos.
    if (RHI::Device* dev = RHI::device()) {
        RHI::RenderTargetDesc desc;
        desc.width       = width;
        desc.height      = height;
        desc.colorFormats = { RHI::Format::RGBA16F };
        desc.hasDepth    = true;
        desc.depthFormat = RHI::Format::D24;
        m_pass       = dev->createRenderTarget(desc);
        FBO          = dev->nativeFramebuffer(m_pass);
        colorTexture = dev->nativeTexture(dev->getColorTexture(m_pass));

        // Inicializa a negro transparente: RGBA16F sin inicializar puede traer NaN/Inf en VRAM
        // (AMD) y provocar fallos de GPU al muestrearlo. Igual que en la ruta previa.
        glBindFramebuffer(GL_FRAMEBUFFER, FBO);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    // Ruta de compatibilidad (editor/headless sin device): GL directo, idéntico al comportamiento previo.
    glGenFramebuffers(1, &FBO);
    glBindFramebuffer(GL_FRAMEBUFFER, FBO);

    glGenTextures(1, &colorTexture);
    glBindTexture(GL_TEXTURE_2D, colorTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTexture, 0);

    glGenRenderbuffers(1, &rboDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, rboDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rboDepth);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        HARUKA_MOTOR_ERROR(ErrorCode::RENDER_TARGET_FAILED, "RenderTarget framebuffer incomplete!");
    }

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
    if (RHI::valid(m_pass)) {
        if (RHI::Device* dev = RHI::device()) dev->destroy(m_pass);   // libera FBO+color+depth del RHI
    } else {
        glDeleteFramebuffers(1, &FBO);
        glDeleteTextures(1, &colorTexture);
        glDeleteRenderbuffers(1, &rboDepth);
    }
}

}} // namespace Haruka::Renderer
