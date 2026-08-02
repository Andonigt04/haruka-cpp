#include "bloom.h"
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"

namespace Haruka { namespace Renderer {

Bloom::Bloom(unsigned int width, unsigned int height) : width(width), height(height) {
    setupFramebuffer();
}

void Bloom::setupFramebuffer() {
    if (RHI::Device* dev = RHI::device()) {
        RHI::RenderTargetDesc d;
        d.width = width; d.height = height;
        d.colorFormats = { RHI::Format::RGBA16F, RHI::Format::RGBA16F };
        d.hasDepth = false;
        m_pass    = dev->createRenderTarget(d);
        m_bright  = dev->getColorTexture(m_pass, 0);
        m_blurred = dev->getColorTexture(m_pass, 1);
    }
}

Bloom::~Bloom() {
    if (RHI::valid(m_pass)) {
        if (RHI::Device* dev = RHI::device()) dev->destroy(m_pass);
    }
}

}} // namespace Haruka::Renderer
