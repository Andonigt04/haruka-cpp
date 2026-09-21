#include "renderer/fallback_texture.h"
#include "rhi/rhi_device.h"

#include <array>

namespace Haruka::Renderer {

Haruka::RHI::TextureHandle fallbackTexture(FallbackTex kind) {
    static std::array<RHI::TextureHandle, (size_t)FallbackTex::Count> s_tex{};
    const size_t k = (size_t)kind;
    if (k >= s_tex.size()) return {};
    if (RHI::valid(s_tex[k])) return s_tex[k];
    RHI::Device* dev = RHI::device();
    if (!dev) return {};

    uint8_t px[4] = { 255, 255, 255, 255 };
    switch (kind) {
        case FallbackTex::Normal:   px[0] = 128; px[1] = 128; px[2] = 255; break;
        case FallbackTex::Metallic: px[0] = px[1] = px[2] = 0;             break;  // 0 = NO metal
        default: break;                                                            // blanco
    }
    RHI::TextureDesc td;
    td.width = 1; td.height = 1;
    td.format = RHI::Format::RGBA8;
    td.filter = RHI::Filter::Nearest;
    td.initialData = px;
    s_tex[k] = dev->createTexture(td);
    return s_tex[k];
}

} // namespace Haruka::Renderer
