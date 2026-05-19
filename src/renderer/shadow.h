/**
 * @file shadow.h
 * @brief Directional shadow map wrapper (depth FBO + depth texture).
 */
#ifndef SHADOW_H
#define SHADOW_H

#include <glad/glad.h>
#include <glm/glm.hpp>

/**
 * @brief Directional shadow map wrapper.
 *
 * Owns a depth framebuffer and a single depth texture.
 */
class Shadow
{
public:
    /** @brief Depth framebuffer object. */
    unsigned int depthMapFBO;
    /** @brief Depth texture attachment. */
    unsigned int depthMap;
    /** @brief Shadow map width in pixels. */
    unsigned int shadowWidth, shadowHeight;

    /** @brief Creates a shadow map with the requested resolution. */
    Shadow(unsigned int width = 1024, unsigned int height = 1024);
    ~Shadow();

    /** @brief Binds the framebuffer for shadow depth rendering. */
    void bindForWriting();
    /** @brief Binds the depth texture for sampling. */
    void bindForReading(unsigned int textureUnit = 2);
    /** @brief Restores default framebuffer binding. */
    void unbind();
private:
    /** @brief Allocates FBO/texture objects. */
    void setupFramebuffer();
};
#endif