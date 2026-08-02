#pragma once
#include "rhi/rhi_types.h"

namespace Haruka { namespace Renderer {

class HDR {
public:
    HDR(unsigned int width, unsigned int height);
    ~HDR();

    Haruka::RHI::TextureHandle getColorTextureHandle() const { return m_colorTex; }
    Haruka::RHI::TextureHandle getBrightTextureHandle() const { return m_brightTex; }
    Haruka::RHI::RenderPassHandle getPass() const { return m_pass; }

private:
    void setupFramebuffer();
    Haruka::RHI::RenderPassHandle m_pass;
    Haruka::RHI::TextureHandle m_colorTex;
    Haruka::RHI::TextureHandle m_brightTex;
    unsigned int width, height;
};

}} // namespace Haruka::Renderer

using Haruka::Renderer::HDR;
namespace Haruka { using Renderer::HDR; }
