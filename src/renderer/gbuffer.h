#pragma once
#include "rhi/rhi_types.h"

namespace Haruka { namespace Renderer {

class GBuffer {
public:
    GBuffer(unsigned int width, unsigned int height);
    ~GBuffer();

    Haruka::RHI::RenderPassHandle getPass() const { return m_pass; }
    Haruka::RHI::TextureHandle getPositionTex() const { return m_position; }
    Haruka::RHI::TextureHandle getNormalTex() const { return m_normal; }
    Haruka::RHI::TextureHandle getAlbedoSpecTex() const { return m_albedoSpec; }
    Haruka::RHI::TextureHandle getEmissiveTex() const { return m_emissive; }

private:
    void setupFramebuffer();
    Haruka::RHI::RenderPassHandle m_pass;
    Haruka::RHI::TextureHandle m_position, m_normal, m_albedoSpec, m_emissive;
    unsigned int width, height;
};

}} // namespace Haruka::Renderer

using Haruka::Renderer::GBuffer;
namespace Haruka { using Renderer::GBuffer; }
