#pragma once
#include "rhi/rhi_types.h"

namespace Haruka { namespace Renderer {

class RenderTarget {
public:
    RenderTarget(unsigned int width, unsigned int height);
    ~RenderTarget();

    Haruka::RHI::TextureHandle getColorTextureHandle() const { return m_colorTex; }
    Haruka::RHI::RenderPassHandle getPass() const { return m_pass; }

    /** @brief Native GL handle for editor integration (escape hatch). */
    unsigned int getFBO() const { return m_fbo; }
    unsigned int getColorTextureGL() const { return m_colorTexGL; }

private:
    void setupFramebuffer();
    Haruka::RHI::RenderPassHandle m_pass;
    Haruka::RHI::TextureHandle m_colorTex;
    unsigned int width, height;
    unsigned int m_fbo = 0;
    unsigned int m_colorTexGL = 0;
};

}} // namespace Haruka::Renderer

using Haruka::Renderer::RenderTarget;
namespace Haruka { using Renderer::RenderTarget; }
