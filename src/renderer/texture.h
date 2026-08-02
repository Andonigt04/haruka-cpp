#ifndef TEXTURE_H
#define TEXTURE_H

#include "rhi/rhi_types.h"

namespace Haruka { namespace Renderer {

class Texture {
public:
    unsigned int ID;
    int width, height, nrChannels;

    Texture(const char* path);
    void cleanup();

    void use(int slot) const;

    Haruka::RHI::TextureHandle handle() const { return m_handle; }

    static void setQuality(float maxAnisotropy, float lodBias);

private:
    static float s_maxAnisotropy;
    static float s_lodBias;
    Haruka::RHI::TextureHandle m_handle;
};

}} // namespace Haruka::Renderer

using Haruka::Renderer::Texture;
namespace Haruka { using Renderer::Texture; }
#endif
