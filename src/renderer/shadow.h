#ifndef SHADOW_H
#define SHADOW_H

#include <glm/glm.hpp>
#include "rhi/rhi_types.h"

namespace Haruka { namespace Renderer {

class Shadow {
public:
    unsigned int shadowWidth, shadowHeight;

    Shadow(unsigned int width = 1024, unsigned int height = 1024);
    ~Shadow();

    void bindForWriting();

    Haruka::RHI::RenderPassHandle pass() const { return m_pass; }
    Haruka::RHI::TextureHandle depthTexture() const { return m_depthTex; }

private:
    void setupFramebuffer();
    Haruka::RHI::RenderPassHandle m_pass;
    Haruka::RHI::TextureHandle m_depthTex;
};

}} // namespace Haruka::Renderer

using Haruka::Renderer::Shadow;
namespace Haruka { using Renderer::Shadow; }
#endif
