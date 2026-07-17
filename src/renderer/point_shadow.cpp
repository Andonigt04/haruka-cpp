#include "point_shadow.h"
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"
#include <iostream>

namespace Haruka { namespace Renderer {

PointShadow::PointShadow(unsigned int resolution) : resolution(resolution)
{
    setupFramebuffer();
}

void PointShadow::setupFramebuffer()
{
    // Ruta RHI: cubemap de profundidad (omni shadow), depth-only.
    if (RHI::Device* dev = RHI::device()) {
        RHI::RenderTargetDesc d;
        d.width = resolution; d.height = resolution;
        d.hasDepth = true; d.depthFormat = RHI::Format::D32F;
        d.depthCube = true; d.depthFilter = RHI::Filter::Nearest;
        m_pass       = dev->createRenderTarget(d);
        FBO          = dev->nativeFramebuffer(m_pass);
        depthCubemap = dev->nativeTexture(dev->getDepthTexture(m_pass));
    }
}

void PointShadow::bindForWriting()
{
    glBindFramebuffer(GL_FRAMEBUFFER, FBO);
    glViewport(0, 0, resolution, resolution);
    // Proyección PROPIA (no invertida) → convención estándar: lejos = 1, test LESS.
    glClearDepth(1.0);
    glDepthFunc(GL_LESS);
    glClear(GL_DEPTH_BUFFER_BIT);
}

void PointShadow::unbind()
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void PointShadow::bindForReading(unsigned int textureUnit)
{
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_CUBE_MAP, depthCubemap);
}

PointShadow::~PointShadow()
{
    if (RHI::valid(m_pass)) {
        if (RHI::Device* dev = RHI::device()) dev->destroy(m_pass);
    }
}

}} // namespace Haruka::Renderer
