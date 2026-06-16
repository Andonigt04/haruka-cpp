#include "texture.h"

#include "stb_image.h"
#include <iostream>
#include <algorithm>
#include "tools/error_reporter.h"

namespace Haruka { namespace Renderer {

float Texture::s_maxAnisotropy = 1.0f;
float Texture::s_lodBias       = 0.0f;

void Texture::setQuality(float maxAnisotropy, float lodBias) {
    s_maxAnisotropy = (maxAnisotropy < 1.0f) ? 1.0f : maxAnisotropy;
    s_lodBias       = lodBias;
}

Texture::Texture(const char* path)
{
    glGenTextures(1, &ID);
    glBindTexture(GL_TEXTURE_2D, ID);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Texture-quality params (GraphicsSettings::textureQuality). Anisotropy and a
    // mip LOD bias are the cheap, visible knobs — no re-upload needed.
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, s_lodBias);
    if (s_maxAnisotropy > 1.0f) {
        float maxSupported = 1.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxSupported);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY,
                        std::min(s_maxAnisotropy, maxSupported));
    }

    unsigned char *data = stbi_load(path, &width, &height, &nrChannels, 0);

    if (data)
    {
        GLenum format = GL_RGB;
        if (nrChannels == 4) format = GL_RGBA;

        // Load data in GPU
        glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    else {
        HARUKA_RENDERER_ERROR(ErrorCode::TEXTURE_LOAD_FAILED,
            std::string("Failed to load texture: ") + path);
    }

    // Clean CPU memory
    stbi_image_free(data);
};

void Texture::use(unsigned int unit)
{
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, ID);
}

void Texture::cleanup()
{
    glDeleteTextures(1, &ID);
}

}} // namespace Haruka::Renderer