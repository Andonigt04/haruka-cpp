#include "texture.h"
#include "stb_image.h"
#include <iostream>
#include <algorithm>
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"

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

    RHI::Device* dev = RHI::device();
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
    ID = dev->nativeTexture(m_handle);

    stbi_image_free(data);
}

void Texture::use(int slot) const {
    RHI::Device* dev = RHI::device();
    if (dev) {
        RHI::Context* ctx = dev->beginFrame();
        ctx->bindTexture((uint32_t)slot, m_handle);
    }
}

void Texture::cleanup()
{
    if (RHI::valid(m_handle)) {
        if (RHI::Device* dev = RHI::device()) dev->destroy(m_handle);
        m_handle = {};
    }
    ID = 0;
}

}} // namespace Haruka::Renderer
