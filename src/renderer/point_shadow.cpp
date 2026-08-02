#include "point_shadow.h"
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"

namespace Haruka { namespace Renderer {

PointShadow::PointShadow(unsigned int resolution) : resolution(resolution) {
    setupFramebuffer();
}

void PointShadow::setupFramebuffer() {
    if (RHI::Device* dev = RHI::device()) {
        RHI::RenderTargetDesc d;
        d.width = resolution; d.height = resolution;
        d.hasDepth = true; d.depthFormat = RHI::Format::D32F;
        d.depthCube = true; d.depthFilter = RHI::Filter::Nearest;
        m_pass     = dev->createRenderTarget(d);
        m_depthTex = dev->getDepthTexture(m_pass);
    }
}

void PointShadow::bindForWriting() {
    // RHI: caller should use Context::beginRenderPass(m_pass, clear)
}

void PointShadow::unbind() {
    // RHI: caller should use Context::endRenderPass()
}

void PointShadow::bindForReading(unsigned int textureUnit) {
    // RHI: caller should use Context::bindTexture(textureUnit, m_depthTex)
}

PointShadow::~PointShadow() {
    if (RHI::valid(m_pass)) {
        if (RHI::Device* dev = RHI::device()) dev->destroy(m_pass);
    }
}

}} // namespace Haruka::Renderer