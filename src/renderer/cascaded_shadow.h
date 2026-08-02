#pragma once

#include <glm/glm.hpp>
#include <vector>
#include "rhi/rhi_types.h"

namespace Haruka { namespace Renderer {

class CascadedShadowMap {
public:
    static constexpr int NUM_CASCADES = 4;
    static constexpr int SHADOW_MAP_RESOLUTION = 2048;

    CascadedShadowMap();
    ~CascadedShadowMap();

    void init(float zNear, float zFar, float lambda = 0.5f);

    void updateCascades(
        const glm::vec3& lightDir,
        const glm::vec3& cameraPos,
        const glm::vec3& cameraForward,
        const glm::vec3& cameraUp,
        float aspect,
        float zNear,
        float zFar,
        float fov
    );

    glm::mat4 getCascadeMatrix(int cascade) const;

    Haruka::RHI::TextureHandle getDepthTexture(int cascade) const;
    Haruka::RHI::RenderPassHandle getPass(int cascade) const;
    void bindForWriting(int cascade) const;
    void bindForReading(int cascade, unsigned int textureUnit) const;

    struct CascadeInfo {
        float zNear;
        float zFar;
        glm::mat4 viewProj;
    };

    CascadeInfo getCascadeInfo(int cascade) const;

    int getNumCascades() const { return NUM_CASCADES; }
    int getShadowMapResolution() const { return SHADOW_MAP_RESOLUTION; }

private:
    std::vector<Haruka::RHI::RenderPassHandle> m_passes;
    std::vector<Haruka::RHI::TextureHandle> m_depthTex;
    std::vector<CascadeInfo> cascades;

    float zNear, zFar, lambda;

    void createShadowMap(int cascade);
};

}} // namespace Haruka::Renderer

using Haruka::Renderer::CascadedShadowMap;
namespace Haruka { using Renderer::CascadedShadowMap; }