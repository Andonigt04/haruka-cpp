#include "hdr.h"
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"

namespace Haruka { namespace Renderer {

HDR::HDR(unsigned int width, unsigned int height) : width(width), height(height) {
    setupFramebuffer();
}

void HDR::setupFramebuffer() {
    if (RHI::Device* dev = RHI::device()) {
        RHI::RenderTargetDesc d;
        d.width = width; d.height = height;
        d.colorFormats = { RHI::Format::R11G11B10F, RHI::Format::R11G11B10F };
        d.hasDepth = true; d.depthFormat = RHI::Format::D32F;
        m_pass      = dev->createRenderTarget(d);
        m_colorTex  = dev->getColorTexture(m_pass, 0);
        m_brightTex = dev->getColorTexture(m_pass, 1);
    }
}

HDR::~HDR() {
    if (RHI::valid(m_pass)) {
        if (RHI::Device* dev = RHI::device()) dev->destroy(m_pass);
    }
}

}} // namespace Haruka::Renderer
