#include "bloom.h"
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"
#include <iostream>

namespace Haruka { namespace Renderer {

Bloom::Bloom(unsigned int width, unsigned int height) : width(width), height(height)
{
    setupFramebuffer();
}

void Bloom::setupFramebuffer()
{
    // Ruta RHI: MRT de 2 color (RGBA16F: bright + blurred), sin depth.
    if (RHI::Device* dev = RHI::device()) {
        RHI::RenderTargetDesc d;
        d.width = width; d.height = height;
        d.colorFormats = { RHI::Format::RGBA16F, RHI::Format::RGBA16F };
        d.hasDepth = false;
        m_pass         = dev->createRenderTarget(d);
        bloomFBO       = dev->nativeFramebuffer(m_pass);
        brightTexture  = dev->nativeTexture(dev->getColorTexture(m_pass, 0));
        blurredTexture = dev->nativeTexture(dev->getColorTexture(m_pass, 1));
    }
}

void Bloom::bindForWriting()
{
    glBindFramebuffer(GL_FRAMEBUFFER, bloomFBO);
    glViewport(0, 0, width, height);
}

void Bloom::unbind()
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Bloom::bindForReading(unsigned int textureUnit, int index)
{
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_2D, index == 0 ? brightTexture : blurredTexture);
}

Bloom::~Bloom()
{
    if (RHI::valid(m_pass)) {
        if (RHI::Device* dev = RHI::device()) dev->destroy(m_pass);
    }
}

}} // namespace Haruka::Renderer
