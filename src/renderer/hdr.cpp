#include "hdr.h"
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"

#include <iostream>

namespace Haruka { namespace Renderer {

HDR::HDR(unsigned int width, unsigned int height) : width(width), height(height)
{
    setupFramebuffer();
}

void HDR::setupFramebuffer()
{
    // Ruta RHI: MRT de 2 color (R11G11B10F: color + bright) + depth. Cachea los ids GL nativos.
    if (RHI::Device* dev = RHI::device()) {
        RHI::RenderTargetDesc d;
        d.width = width; d.height = height;
        d.colorFormats = { RHI::Format::R11G11B10F, RHI::Format::R11G11B10F };
        d.hasDepth = true; d.depthFormat = RHI::Format::D32F;   // reversed-Z necesita depth float
        m_pass        = dev->createRenderTarget(d);
        hdrFBO        = dev->nativeFramebuffer(m_pass);
        m_colorTex    = dev->getColorTexture(m_pass, 0);   // handle (ruta PSO/Context)
        colorTexture  = dev->nativeTexture(m_colorTex);    // id GL (crutch de la transición)
        brightTexture = dev->nativeTexture(dev->getColorTexture(m_pass, 1));
    }
}

void HDR::bindForWriting()
{
    glBindFramebuffer(GL_FRAMEBUFFER, hdrFBO);
    glViewport(0, 0, width, height);
}

void HDR::unbind()
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void HDR::bindForReading(unsigned int textureUnit, int index)
{
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_2D, index == 0 ? colorTexture : brightTexture);
}

HDR::~HDR()
{
    if (RHI::valid(m_pass)) {
        if (RHI::Device* dev = RHI::device()) dev->destroy(m_pass);
    }
}

}} // namespace Haruka::Renderer
