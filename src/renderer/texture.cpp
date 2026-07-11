#include "texture.h"

#include "stb_image.h"
#include <iostream>
#include <algorithm>
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"

namespace Haruka { namespace Renderer {

float Texture::s_maxAnisotropy = 1.0f;
float Texture::s_lodBias       = 0.0f;

void Texture::setQuality(float maxAnisotropy, float lodBias) {
    s_maxAnisotropy = (maxAnisotropy < 1.0f) ? 1.0f : maxAnisotropy;
    s_lodBias       = lodBias;
}

Texture::Texture(const char* path)
{
    unsigned char* data = stbi_load(path, &width, &height, &nrChannels, 0);
    if (!data) {
        HARUKA_RENDERER_ERROR(ErrorCode::TEXTURE_LOAD_FAILED,
            std::string("Failed to load texture: ") + path);
        return;
    }

    // Ruta RHI: si hay device, la textura se crea a través del backend (GL o, en el futuro, Vulkan).
    if (RHI::Device* dev = RHI::device()) {
        RHI::TextureDesc desc;
        desc.width         = (uint32_t)width;
        desc.height        = (uint32_t)height;
        desc.format        = (nrChannels == 4) ? RHI::Format::RGBA8 : RHI::Format::RGB8;
        desc.filter        = RHI::Filter::Linear;
        desc.wrap          = RHI::Wrap::Repeat;
        desc.mipmaps       = true;
        desc.maxAnisotropy = s_maxAnisotropy;
        desc.lodBias       = s_lodBias;
        desc.initialData   = data;
        m_handle = dev->createTexture(desc);
        ID = dev->nativeTexture(m_handle);   // id GL nativo, para use()/ImGui durante la transición
    }
    // Ruta de compatibilidad (editor/headless sin device): GL directo, comportamiento idéntico al previo.
    else {
        glGenTextures(1, &ID);
        glBindTexture(GL_TEXTURE_2D, ID);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, s_lodBias);
        if (s_maxAnisotropy > 1.0f) {
            float maxSupported = 1.0f;
            glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxSupported);
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY,
                            std::min(s_maxAnisotropy, maxSupported));
        }
        GLenum format = (nrChannels == 4) ? GL_RGBA : GL_RGB;
        glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    stbi_image_free(data);
};

void Texture::use(unsigned int unit)
{
    // Bind directo por unidad; ID es el id GL nativo venga de la ruta que venga.
    // (El bind se migrará al Context del RHI en F3.)
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, ID);
}

void Texture::cleanup()
{
    if (RHI::valid(m_handle)) {
        if (RHI::Device* dev = RHI::device()) dev->destroy(m_handle);
        m_handle = {};
    } else if (ID) {
        glDeleteTextures(1, &ID);   // ruta de compatibilidad
    }
    ID = 0;
}

}} // namespace Haruka::Renderer
