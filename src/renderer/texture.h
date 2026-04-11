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
};
#endif