#pragma once
#include <glad/glad.h>

/**
 * @brief Color render target wrapper (FBO + color texture + depth RBO).
 */
class RenderTarget {
public:
    /** @brief Creates target resources with fixed dimensions. */
    RenderTarget(unsigned int width, unsigned int height);
    ~RenderTarget();

    /** @brief Binds FBO for draw/write operations. */
    void bindForWriting();
    /** @brief Restores default framebuffer binding. */
    void unbind();
    /** @brief Binds color texture for shader read at texture unit. */
    void bindForReading(unsigned int textureUnit);

    /** @brief Returns color attachment texture id. */
    unsigned int getColorTexture() const { return colorTexture; }
    /** @brief Returns framebuffer object id. */
    unsigned int getFBO() const { return FBO; }

private:
    void setupFramebuffer();

    unsigned int FBO = 0;
    unsigned int colorTexture = 0;
    unsigned int rboDepth = 0;
    unsigned int width, height;
};