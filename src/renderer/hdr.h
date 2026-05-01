/**
 * @file hdr.h
 * @brief HDR framebuffer with color and bright-pass attachments for tone mapping.
 */
#pragma once

#include <glad/glad.h>

/**
 * @brief HDR framebuffer wrapper with color and bright attachments.
 */
class HDR
{
public:
    /** @brief Creates HDR framebuffer at the requested resolution. */
    HDR(unsigned int width, unsigned int height);
    ~HDR();

    /** @brief Binds HDR FBO for scene rendering. */
    void bindForWriting();
    /** @brief Restores default framebuffer binding. */
    void unbind();
    /** @brief Binds HDR attachments for post-processing reads. */
    void bindForReading(unsigned int textureUnit, int index);

    /** @brief Returns HDR color texture id. */
    unsigned int getColorTexture() { return colorTexture; }
    /** @brief Returns HDR bright-pass texture id. */
    unsigned int getBrightTexture() { return brightTexture; }
    /** @brief Returns HDR framebuffer id. */
    unsigned int getFBO() { return hdrFBO; }
private:
    /** @brief Allocates framebuffer and attachments. */
    void setupFramebuffer();
    unsigned int hdrFBO;
    unsigned int colorTexture;
    unsigned int brightTexture;
    unsigned int rboDepth;
    unsigned int width, height;
};