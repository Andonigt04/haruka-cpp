#include "shadow.h"
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"

#include <iostream>

namespace Haruka { namespace Renderer {

Shadow::Shadow(unsigned int width, unsigned int height) : shadowWidth(width), shadowHeight(height)
{
    setupFramebuffer();
}

void Shadow::setupFramebuffer()
{
    // Ruta RHI: depth-only, profundidad como TEXTURA muestreable con borde blanco (fuera = sin sombra).
    if (RHI::Device* dev = RHI::device()) {
        RHI::RenderTargetDesc d;
        d.width = shadowWidth; d.height = shadowHeight;
        d.hasDepth = true; d.depthFormat = RHI::Format::D32F;
        d.depthAsTexture = true; d.depthBorderClamp = true; d.depthFilter = RHI::Filter::Nearest;
        m_pass      = dev->createRenderTarget(d);
        depthMapFBO = dev->nativeFramebuffer(m_pass);
        depthMap    = dev->nativeTexture(dev->getDepthTexture(m_pass));
    }
}

void Shadow::bindForWriting()
{
    glBindFramebuffer(GL_FRAMEBUFFER, depthMapFBO);
    glViewport(0, 0, shadowWidth, shadowHeight);
}

void Shadow::bindForReading(unsigned int textureUnit)
{
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_2D, depthMap);
}

void Shadow::unbind()
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

Shadow::~Shadow()
{
    if (RHI::valid(m_pass)) {
        if (RHI::Device* dev = RHI::device()) dev->destroy(m_pass);
    }
}


}} // namespace Haruka::Renderer
