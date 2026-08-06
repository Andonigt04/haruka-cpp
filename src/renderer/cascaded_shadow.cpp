#include "cascaded_shadow.h"
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"
#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <cmath>
#include <limits>
#include <iostream>

namespace Haruka { namespace Renderer {

CascadedShadowMap::CascadedShadowMap() {}

CascadedShadowMap::~CascadedShadowMap() {
    if (RHI::Device* dev = RHI::device()) {
        for (auto& p : m_passes)
            if (RHI::valid(p)) dev->destroy(p);
    }
}

void CascadedShadowMap::init(float zNear, float zFar, float lambda) {
    this->zNear = zNear;
    this->zFar  = zFar;
    this->lambda = lambda;

    m_passes.resize(NUM_CASCADES);
    m_depthTex.resize(NUM_CASCADES);
    cascades.resize(NUM_CASCADES);

    for (int i = 0; i < NUM_CASCADES; ++i)
        createShadowMap(i);
}

void CascadedShadowMap::createShadowMap(int cascade) {
    RHI::Device* dev = RHI::device();
    if (!dev) return;

    RHI::RenderTargetDesc desc;
    desc.width       = SHADOW_MAP_RESOLUTION;
    desc.height      = SHADOW_MAP_RESOLUTION;
    desc.hasDepth    = true;
    desc.depthFormat = RHI::Format::D32F;
    desc.depthFilter = RHI::Filter::Nearest;
    m_passes[cascade] = dev->createRenderTarget(desc);
    m_depthTex[cascade] = dev->getDepthTexture(m_passes[cascade]);
}

void CascadedShadowMap::updateCascades(
    const glm::vec3& lightDir,
    const glm::vec3& cameraPos,
    const glm::vec3& cameraForward,
    const glm::vec3& cameraUp,
    float aspect,
    float cameraNear,
    float cameraFar,
    float fov)
{
    // Compute cascade split depths using the PSSM lambda function
    std::array<float, NUM_CASCADES + 1> splits;
    splits[0] = cameraNear;
    splits[NUM_CASCADES] = cameraFar;

    for (int i = 1; i < NUM_CASCADES; ++i) {
        float ratio = (float)i / (float)NUM_CASCADES;
        float logSplit  = cameraNear * powf(cameraFar / cameraNear, ratio);
        float uniformSplit = cameraNear + (cameraFar - cameraNear) * ratio;
        splits[i] = logSplit * lambda + uniformSplit * (1.0f - lambda);
    }

    // Build light view-projection for each cascade
    for (int i = 0; i < NUM_CASCADES; ++i) {
        float n = splits[i];
        float f = splits[i + 1];

        // Frustum corners in view space
        float tanHalfFov = tanf(glm::radians(fov * 0.5f));
        float xn = n * tanHalfFov;
        float xf = f * tanHalfFov;
        float yn = xn / aspect;
        float yf = xf / aspect;

        glm::vec3 fc[8] = {
            glm::vec3( xn,  yn, -n), glm::vec3( xf,  yf, -f),
            glm::vec3(-xn,  yn, -n), glm::vec3(-xf,  yf, -f),
            glm::vec3( xn, -yn, -n), glm::vec3( xf, -yf, -f),
            glm::vec3(-xn, -yn, -n), glm::vec3(-xf, -yf, -f),
        };

        // Rotate frustum corners to world space
        glm::mat4 camRot = glm::lookAt(cameraPos, cameraPos + cameraForward, cameraUp);
        glm::mat4 invCamRot = glm::inverse(camRot);

        glm::vec3 center(0.0f);
        glm::vec3 cornersWS[8];
        for (int j = 0; j < 8; ++j) {
            cornersWS[j] = glm::vec3(invCamRot * glm::vec4(fc[j], 1.0f));
            center += cornersWS[j];
        }
        center /= 8.0f;

        // Light view matrix looking at frustum center
        glm::vec3 lightPos = center - lightDir * 200.0f;
        glm::mat4 lightView = glm::lookAt(lightPos, center, glm::vec3(0.0f, 1.0f, 0.0f));

        // Compute AABB in light space
        float minX =  std::numeric_limits<float>::max();
        float maxX = -std::numeric_limits<float>::max();
        float minY =  std::numeric_limits<float>::max();
        float maxY = -std::numeric_limits<float>::max();
        float minZ =  std::numeric_limits<float>::max();
        float maxZ = -std::numeric_limits<float>::max();

        for (int j = 0; j < 8; ++j) {
            glm::vec4 lv = lightView * glm::vec4(cornersWS[j], 1.0f);
            minX = std::min(minX, lv.x); maxX = std::max(maxX, lv.x);
            minY = std::min(minY, lv.y); maxY = std::max(maxY, lv.y);
            minZ = std::min(minZ, lv.z); maxZ = std::max(maxZ, lv.z);
        }

        // Stabilize: snap to texel-sized increments
        float texelSize = (maxX - minX) / SHADOW_MAP_RESOLUTION;
        minX -= fmodf(minX, texelSize);
        maxX -= fmodf(maxX, texelSize);
        minY -= fmodf(minY, texelSize);
        maxY -= fmodf(maxY, texelSize);

        glm::mat4 lightProj = glm::ortho(minX, maxX, minY, maxY, -maxZ, -minZ);
        cascades[i].zNear = splits[i];
        cascades[i].zFar  = splits[i + 1];
        cascades[i].viewProj = lightProj * lightView;
    }
}

glm::mat4 CascadedShadowMap::getCascadeMatrix(int cascade) const {
    return (cascade >= 0 && cascade < NUM_CASCADES) ? cascades[cascade].viewProj : glm::mat4(1.0f);
}

Haruka::RHI::TextureHandle CascadedShadowMap::getDepthTexture(int cascade) const {
    return (cascade >= 0 && cascade < NUM_CASCADES) ? m_depthTex[cascade] : Haruka::RHI::TextureHandle{};
}

Haruka::RHI::RenderPassHandle CascadedShadowMap::getPass(int cascade) const {
    return (cascade >= 0 && cascade < NUM_CASCADES) ? m_passes[cascade] : Haruka::RHI::RenderPassHandle{};
}

CascadedShadowMap::CascadeInfo CascadedShadowMap::getCascadeInfo(int cascade) const {
    return (cascade >= 0 && cascade < NUM_CASCADES) ? cascades[cascade] : CascadeInfo{};
}

void CascadedShadowMap::bindForWriting(int cascade) const {
    // RHI path: caller should use Context::beginRenderPass(m_passes[cascade], ...)
    // This method kept for API compatibility but does nothing — migrate callers.
}

void CascadedShadowMap::bindForReading(int cascade, unsigned int textureUnit) const {
    // RHI path: caller should use Context::bindTexture(textureUnit, m_depthTex[cascade])
    // This method kept for API compatibility but does nothing — migrate callers.
}

}} // namespace Haruka::Renderer