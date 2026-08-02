#pragma once
#include "rhi/rhi_types.h"

namespace Haruka { namespace Renderer {

class Bloom {
public:
    Bloom(unsigned int width, unsigned int height);
    ~Bloom();

    Haruka::RHI::RenderPassHandle getPass() const { return m_pass; }
    Haruka::RHI::TextureHandle getBrightTexture() const { return m_bright; }
    Haruka::RHI::TextureHandle getBlurredTexture() const { return m_blurred; }

private:
    void setupFramebuffer();
    Haruka::RHI::RenderPassHandle m_pass;
    Haruka::RHI::TextureHandle m_bright, m_blurred;
    unsigned int width, height;
};

}} // namespace Haruka::Renderer

using Haruka::Renderer::Bloom;
namespace Haruka { using Renderer::Bloom; }
