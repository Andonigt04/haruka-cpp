#pragma once
#include <glad/glad.h>

/**
 * @brief Bloom post-processing buffer pair.
 *
 * Stores a bright-pass texture and a blurred texture used during composition.
 */
class Bloom
{
public:
    /** @brief Creates bloom buffers at the given resolution. */
    Bloom(unsigned int width, unsigned int height);
    ~Bloom();
    
    /** @brief Binds bloom FBO for writing. */
    void bindForWriting();
    /** @brief Restores default framebuffer binding. */
    void unbind();
    /** @brief Binds one bloom texture for reading. */
    void bindForReading(unsigned int textureUnit, int index);  // 0=bright, 1=blurred
    
    /** @brief Returns bright-pass texture id. */
    unsigned int getBrightTexture() { return brightTexture; }
    /** @brief Returns blurred bloom texture id. */
    unsigned int getBlurredTexture() { return blurredTexture; }
    
private:
    /** @brief Allocates framebuffer and attachments. */
    void setupFramebuffer();
    unsigned int bloomFBO;
    unsigned int brightTexture;
    unsigned int blurredTexture;
    unsigned int rboDepth;
    unsigned int width, height;
};