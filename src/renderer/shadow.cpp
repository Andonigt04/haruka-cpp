#include "shadow.h"
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"

namespace Haruka { namespace Renderer {

Shadow::Shadow(unsigned int width, unsigned int height) : shadowWidth(width), shadowHeight(height) {
    setupFramebuffer();
}

void Shadow::bindForWriting() {
    RHI::Device* dev = RHI::device();
    if (dev && RHI::valid(m_pass)) {
        RHI::ClearValues clear{};
        clear.clearDepth = true;
        clear.depth = 0.0;
        dev->beginFrame()->beginRenderPass(m_pass, clear);
    }
}

void Shadow::setupFramebuffer() {
    if (RHI::Device* dev = RHI::device()) {
        RHI::RenderTargetDesc d;
        d.width = shadowWidth; d.height = shadowHeight;
        d.hasDepth = true; d.depthFormat = RHI::Format::D32F;
        d.depthAsTexture = true; d.depthBorderClamp = true; d.depthFilter = RHI::Filter::Nearest;
        m_pass = dev->createRenderTarget(d);
        m_depthTex = dev->getDepthTexture(m_pass);
    }
}

Shadow::~Shadow() {
    if (RHI::valid(m_pass)) {
        if (RHI::Device* dev = RHI::device()) dev->destroy(m_pass);
    }
}

}} // namespace Haruka::Renderer
