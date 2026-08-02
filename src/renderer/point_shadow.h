#pragma once
#include <glm/glm.hpp>
#include "rhi/rhi_types.h"

namespace Haruka { namespace Renderer {

class PointShadow {
public:
    PointShadow(unsigned int resolution = 1024);
    ~PointShadow();

    Haruka::RHI::TextureHandle getDepthCubemap() const { return m_depthTex; }
    Haruka::RHI::RenderPassHandle getPass() const { return m_pass; }
    void bindForWriting();
    void unbind();
    void bindForReading(unsigned int textureUnit);

private:
    void setupFramebuffer();

    Haruka::RHI::RenderPassHandle m_pass;
    Haruka::RHI::TextureHandle m_depthTex;
    unsigned int resolution;
};

}} // namespace Haruka::Renderer

using Haruka::Renderer::PointShadow;
namespace Haruka { using Renderer::PointShadow; }