#include "renderer/terrain_renderer.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"
#include "stb_image.h"

#include <cstdio>
#include <string>
#include "core/logger.h"

namespace Haruka {

TerrainRenderer::~TerrainRenderer() {
    // RHI textures are managed by the device; no explicit cleanup needed here.
}

static Haruka::RHI::TextureHandle loadTexture(const std::string& path) {
    if (path.empty()) return {};
    auto* dev = RHI::device();
    if (!dev) return {};
    int w = 0, h = 0, ch = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!data) {
        HARUKA_LOGW("TerrainRenderer", "Failed to load texture: %s", path.c_str());
        return {};
    }
    RHI::TextureDesc desc;
    desc.width       = (uint32_t)w;
    desc.height      = (uint32_t)h;
    desc.format      = RHI::Format::RGBA8;
    desc.filter      = RHI::Filter::Linear;
    desc.wrap        = RHI::Wrap::Repeat;
    desc.mipmaps     = true;
    desc.initialData = data;
    RHI::TextureHandle tex = dev->createTexture(desc);
    stbi_image_free(data);
    return tex;
}

TerrainRenderer::BiomeTextures
TerrainRenderer::loadTextures(const std::string& albedoPath,
                              const std::string& normalPath,
                              const std::string& heightPath,
                              float /*tiling*/) {
    BiomeTextures bt{};
    bt.sandAlbedo   = loadTexture(albedoPath);
    bt.sandNormal   = loadTexture(normalPath);
    bt.grassAlbedo  = loadTexture(albedoPath);
    bt.grassNormal  = loadTexture(normalPath);
    bt.landAlbedo   = loadTexture(albedoPath);
    bt.landNormal   = loadTexture(normalPath);
    bt.rockAlbedo   = loadTexture(albedoPath);
    bt.rockNormal   = loadTexture(normalPath);
    return bt;
}

} // namespace Haruka
