#include "gbuffer.h"
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"
#include <iostream>

namespace Haruka { namespace Renderer {

GBuffer::GBuffer(unsigned int width, unsigned int height)
    : width(width), height(height) {
    setupFramebuffer();
}

void GBuffer::setupFramebuffer() {
    // Ruta RHI: MRT de 4 color (pos/normal RGBA16F, albedoSpec/emissive RGBA8) + depth. Filtro nearest.
    if (RHI::Device* dev = RHI::device()) {
        RHI::RenderTargetDesc d;
        d.width = width; d.height = height;
        d.colorFormats = { RHI::Format::RGBA16F, RHI::Format::RGBA16F, RHI::Format::RGBA8, RHI::Format::RGBA8 };
        d.colorFilter = RHI::Filter::Nearest;
        d.hasDepth = true; d.depthFormat = RHI::Format::D24;
        m_pass      = dev->createRenderTarget(d);
        gBufferFBO  = dev->nativeFramebuffer(m_pass);
        gPosition   = dev->nativeTexture(dev->getColorTexture(m_pass, 0));
        gNormal     = dev->nativeTexture(dev->getColorTexture(m_pass, 1));
        gAlbedoSpec = dev->nativeTexture(dev->getColorTexture(m_pass, 2));
        gEmissive   = dev->nativeTexture(dev->getColorTexture(m_pass, 3));
    }
}

void GBuffer::bindForWriting() {
    glBindFramebuffer(GL_FRAMEBUFFER, gBufferFBO);
    glViewport(0, 0, width, height);
}

void GBuffer::unbind() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GBuffer::bindForReading(int index, unsigned int textureUnit) {
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    if (index == 0) glBindTexture(GL_TEXTURE_2D, gPosition);
    if (index == 1) glBindTexture(GL_TEXTURE_2D, gNormal);
    if (index == 2) glBindTexture(GL_TEXTURE_2D, gAlbedoSpec);
    if (index == 3) glBindTexture(GL_TEXTURE_2D, gEmissive);
}

GBuffer::~GBuffer() {
    if (RHI::valid(m_pass)) {
        if (RHI::Device* dev = RHI::device()) dev->destroy(m_pass);
    }
}

}} // namespace Haruka::Renderer
