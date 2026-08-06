#pragma once

#include <string>
#include <unordered_set>
#include <cstdint>
#include <glm/glm.hpp>
#include "rhi/rhi_types.h"

namespace Haruka::RHI { class Device; }

namespace Haruka {

class TerrainRenderer {
public:
    struct BiomeTextures {
        Haruka::RHI::TextureHandle sandAlbedo, sandNormal;
        Haruka::RHI::TextureHandle grassAlbedo, grassNormal;
        Haruka::RHI::TextureHandle landAlbedo, landNormal;
        Haruka::RHI::TextureHandle rockAlbedo, rockNormal;
    };

    TerrainRenderer() = default;
    ~TerrainRenderer();

    /// Load terrain textures from a planet's surface config paths, into BiomeTextures.
    BiomeTextures loadTextures(const std::string& albedoPath,
                               const std::string& normalPath,
                               const std::string& heightPath,
                               float tiling);

    void setBiomeTextures(const BiomeTextures& t, bool has, bool hasRock = false) {
        m_biome = t;
        m_hasTex = has;
        m_hasRock = hasRock;
    }

    const BiomeTextures& getBiomeTextures() const { return m_biome; }
    bool hasTextures() const { return m_hasTex; }
    bool hasRockTexture() const { return m_hasRock; }

    /// Stub: old terrain system accessor — always returns nullptr.
    const std::unordered_set<uint64_t>* drawnHashesFor(const std::string&) const { return nullptr; }

private:
    BiomeTextures m_biome{};
    bool m_hasTex = false;
    bool m_hasRock = false;
};

} // namespace Haruka
