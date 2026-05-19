/**
 * @file gbuffer.h
 * @brief Deferred rendering G-buffer (position, normal, albedo/spec, emissive, depth).
 */
#pragma once
#include <glad/glad.h>

/**
 * @brief Deferred shading geometry buffer wrapper.
 *
 * Manages position, normal, albedo/spec, emissive attachments and depth.
 */
class GBuffer {
public:
    /** @brief Allocates framebuffer and attachments with fixed dimensions. */
    GBuffer(unsigned int width, unsigned int height);
    ~GBuffer();

    /** @brief Binds G-buffer FBO for geometry pass writes. */
    void bindForWriting();
    /** @brief Restores default framebuffer binding. */
    void unbind();
    /** @brief Binds selected attachment texture to a texture unit for reading. */
    void bindForReading(int index, unsigned int textureUnit);

    /** @brief Position attachment texture id. */
    unsigned int getPositionTex() const { return gPosition; }
    /** @brief Normal attachment texture id. */
    unsigned int getNormalTex() const { return gNormal; }
    /** @brief Albedo/spec attachment texture id. */
    unsigned int getAlbedoSpecTex() const { return gAlbedoSpec; }
    /** @brief Emissive attachment texture id. */
    unsigned int getEmissiveTex() const { return gEmissive; }

private:
    void setupFramebuffer();

    unsigned int gBufferFBO = 0;
    unsigned int gPosition = 0;
    unsigned int gNormal = 0;
    unsigned int gAlbedoSpec = 0;
    unsigned int gEmissive = 0;
    unsigned int rboDepth = 0;
    unsigned int width, height;
};