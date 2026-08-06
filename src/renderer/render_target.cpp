#include "render_target.h"
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"

namespace Haruka { namespace Renderer {

RenderTarget::RenderTarget(unsigned int width, unsigned int height)
    : width(width), height(height) {
    setupFramebuffer();
}

void RenderTarget::setupFramebuffer() {
    RHI::Device* dev = RHI::device();
    RHI::RenderTargetDesc desc;
    desc.width       = width;
    desc.height      = height;
    desc.colorFormats = { RHI::Format::RGBA16F };
    desc.hasDepth    = true;
    desc.depthFormat = RHI::Format::D32F;
    m_pass       = dev->createRenderTarget(desc);
    m_colorTex   = dev->getColorTexture(m_pass);
    m_fbo        = dev->nativeFramebuffer(m_pass);
    m_colorTexGL = dev->nativeTexture(m_colorTex);
}

RenderTarget::~RenderTarget() {
    if (RHI::valid(m_pass))
        if (RHI::Device* dev = RHI::device()) dev->destroy(m_pass);
}

}} // namespace Haruka::Renderer
