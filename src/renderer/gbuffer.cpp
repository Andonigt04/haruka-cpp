#include "gbuffer.h"
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"

namespace Haruka { namespace Renderer {

GBuffer::GBuffer(unsigned int width, unsigned int height)
    : width(width), height(height) {
    setupFramebuffer();
}

void GBuffer::setupFramebuffer() {
    if (RHI::Device* dev = RHI::device()) {
        RHI::RenderTargetDesc d;
        d.width = width; d.height = height;
        d.colorFormats = { RHI::Format::RGBA16F, RHI::Format::RGBA16F, RHI::Format::RGBA8, RHI::Format::RGBA8 };
        d.colorFilter = RHI::Filter::Nearest;
        d.hasDepth = true; d.depthFormat = RHI::Format::D24;
        m_pass      = dev->createRenderTarget(d);
        m_position  = dev->getColorTexture(m_pass, 0);
        m_normal    = dev->getColorTexture(m_pass, 1);
        m_albedoSpec = dev->getColorTexture(m_pass, 2);
        m_emissive  = dev->getColorTexture(m_pass, 3);
    }
}

GBuffer::~GBuffer() {
    if (RHI::valid(m_pass)) {
        if (RHI::Device* dev = RHI::device()) dev->destroy(m_pass);
    }
}

}} // namespace Haruka::Renderer
