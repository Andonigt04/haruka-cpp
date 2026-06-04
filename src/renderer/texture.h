/**
 * @file texture.h
 * @brief Basic 2D texture loader and GL binding wrapper.
 */
#ifndef TEXTURE_H
#define TEXTURE_H

#include <glad/glad.h>

/** @brief Basic 2D texture wrapper. */
class Texture {
public:
    /** @brief OpenGL texture id. */
    unsigned int ID;
    /** @brief Loaded image width in pixels. */
    int width, height, nrChannels;

    /** @brief Loads a texture from file path. */
    Texture(const char* path);
    /** @brief Binds texture to a sampler unit. */
    void use(unsigned int unit = 0);
    /** @brief Deletes owned OpenGL texture resources. */
    void cleanup();

    /**
     * @brief Sets the texture-quality params applied to every newly loaded texture.
     *
     * Driven by GraphicsSettings::textureQuality (mapped in applyGraphicsSettings).
     * @param maxAnisotropy Anisotropic filtering level (1 = off; clamped to GL max).
     * @param lodBias       Mip LOD bias; >0 picks blurrier mips (cheaper), <0 sharper.
     *
     * Applies to textures loaded AFTER the call (params are baked at creation), so a
     * live change takes full effect on the next scene/asset (re)load.
     */
    static void setQuality(float maxAnisotropy, float lodBias);

private:
    static float s_maxAnisotropy; // 1 = isotropic
    static float s_lodBias;       // 0 = no bias
};
#endif