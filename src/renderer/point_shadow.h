/**
 * @file point_shadow.h
 * @brief Omnidirectional point-light shadow map using a depth cubemap.
 */
#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include "rhi/rhi_types.h"

namespace Haruka { namespace Renderer {

/**
 * @brief Omnidirectional point-light shadow map wrapper.
 */
class PointShadow {
public:
    /** @brief Creates a point-light shadow cubemap. */
    PointShadow(unsigned int resolution = 1024);
    ~PointShadow();

    /** @brief Binds framebuffer for depth cubemap rendering. */
    void bindForWriting();
    /** @brief Restores default framebuffer binding. */
    void unbind();
    /** @brief Binds depth cubemap for sampling. */
    void bindForReading(unsigned int textureUnit);

    /** @brief Returns depth cubemap texture id. */
    unsigned int getDepthCubemap() const { return depthCubemap; }
    /** @brief Returns framebuffer id. */
    unsigned int getFBO() const { return FBO; }

private:
    /** @brief Allocates framebuffer and cubemap faces. */
    void setupFramebuffer();

    Haruka::RHI::RenderPassHandle m_pass;
    unsigned int FBO = 0;
    unsigned int depthCubemap = 0;
    unsigned int resolution;
};

}} // namespace Haruka::Renderer

using Haruka::Renderer::PointShadow;
namespace Haruka { using Renderer::PointShadow; }