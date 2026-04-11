#include "cascaded_shadow.h"
#include "core/error_reporter.h"
#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <cmath>
#include <limits>
#include <iostream>

CascadedShadowMap::CascadedShadowMap() {}

CascadedShadowMap::~CascadedShadowMap() {
    for (auto fbo : shadowMapFramebuffers) {
        glDeleteFramebuffers(1, &fbo);
    }
    for (auto tex : shadowMapTextures) {
        glDeleteTextures(1, &tex);
    }
}

void CascadedShadowMap::init(float zNear, float zFar, float lambda) {
    this->zNear = zNear;
    this->zFar = zFar;
    this->lambda = lambda;

    shadowMapTextures.resize(NUM_CASCADES);
    shadowMapFramebuffers.resize(NUM_CASCADES);
    cascades.resize(NUM_CASCADES);

    for (int i = 0; i < NUM_CASCADES; i++) {
        createShadowMap(i);
    }

    std::cout << "✓ Cascaded Shadow Maps initialized\n";
    std::cout << "  Cascades: " << NUM_CASCADES << "\n";
    std::cout << "  Resolution: " << SHADOW_MAP_RESOLUTION << "x" << SHADOW_MAP_RESOLUTION << "\n";
}

void CascadedShadowMap::createShadowMap(int cascade) {
    // Crear texture
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F,
                 SHADOW_MAP_RESOLUTION, SHADOW_MAP_RESOLUTION,
                 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

    shadowMapTextures[cascade] = texture;

    // Crear framebuffer
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, texture, 0);
    
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        HARUKA_MOTOR_ERROR(ErrorCode::RENDER_TARGET_FAILED, "Shadow map framebuffer incomplete!");
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    shadowMapFramebuffers[cascade] = fbo;
}

namespace {
glm::vec3 safeNormalize(const glm::vec3& v, const glm::vec3& fallback) {
    float len = glm::length(v);
    if (len < 1e-6f) return fallback;
    return v / len;
}
}

void CascadedShadowMap::updateCascades(
    const glm::vec3& lightDir,
    const glm::vec3& cameraPos,
    const glm::vec3& cameraForward,
    const glm::vec3& cameraUp,
    float aspect,
    float zNear,
    float zFar,
    float fov) {

    this->zNear = zNear;
    this->zFar = zFar;

    glm::vec3 forward = safeNormalize(cameraForward, glm::vec3(0.0f, 0.0f, -1.0f));
    glm::vec3 up = safeNormalize(cameraUp, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::vec3 right = safeNormalize(glm::cross(forward, up), glm::vec3(1.0f, 0.0f, 0.0f));
    up = safeNormalize(glm::cross(right, forward), glm::vec3(0.0f, 1.0f, 0.0f));

    float tanHalfFov = std::tan(glm::radians(fov) * 0.5f);

    // Splits prácticos: mezcla entre lineal y logarítmico
    std::array<float, NUM_CASCADES + 1> splits{};
    splits[0] = zNear;
    for (int i = 1; i <= NUM_CASCADES; ++i) {
        float p = static_cast<float>(i) / static_cast<float>(NUM_CASCADES);
        float logSplit = zNear * std::pow(zFar / zNear, p);
        float uniformSplit = zNear + (zFar - zNear) * p;
        splits[i] = lambda * logSplit + (1.0f - lambda) * uniformSplit;
    }

    for (int i = 0; i < NUM_CASCADES; i++) {
        float nearDist = splits[i];
        float farDist = splits[i + 1];

        glm::vec3 nearCenter = cameraPos + forward * nearDist;
        glm::vec3 farCenter = cameraPos + forward * farDist;

        float nearHeight = nearDist * tanHalfFov;
        float nearWidth = nearHeight * aspect;
        float farHeight = farDist * tanHalfFov;
        float farWidth = farHeight * aspect;

        std::array<glm::vec3, 8> corners = {
            nearCenter - right * nearWidth + up * nearHeight,
            nearCenter + right * nearWidth + up * nearHeight,
            nearCenter + right * nearWidth - up * nearHeight,
            nearCenter - right * nearWidth - up * nearHeight,
            farCenter - right * farWidth + up * farHeight,
            farCenter + right * farWidth + up * farHeight,
            farCenter + right * farWidth - up * farHeight,
            farCenter - right * farWidth - up * farHeight
        };

        glm::vec3 frustumCenter(0.0f);
        for (const auto& c : corners) frustumCenter += c;
        frustumCenter *= 1.0f / 8.0f;

        glm::vec3 lightPos = frustumCenter - safeNormalize(lightDir, glm::vec3(1.0f, 1.0f, 1.0f)) * (farDist * 4.0f);
        glm::vec3 lightUp = std::abs(glm::dot(safeNormalize(lightDir, glm::vec3(1.0f, 1.0f, 1.0f)), glm::vec3(0.0f, 1.0f, 0.0f))) > 0.9f
            ? glm::vec3(0.0f, 0.0f, 1.0f)
            : glm::vec3(0.0f, 1.0f, 0.0f);

        glm::mat4 lightView = glm::lookAt(lightPos, frustumCenter, lightUp);

        glm::vec3 minExtents(std::numeric_limits<float>::max());
        glm::vec3 maxExtents(std::numeric_limits<float>::lowest());
        for (const auto& corner : corners) {
            glm::vec4 tr = lightView * glm::vec4(corner, 1.0f);
            minExtents = glm::min(minExtents, glm::vec3(tr));
            maxExtents = glm::max(maxExtents, glm::vec3(tr));
        }

        const float zMult = 8.0f;
        if (minExtents.z < 0.0f) minExtents.z *= zMult;
        else minExtents.z /= zMult;
        if (maxExtents.z < 0.0f) maxExtents.z /= zMult;
        else maxExtents.z *= zMult;

        glm::mat4 lightProj = glm::ortho(minExtents.x, maxExtents.x, minExtents.y, maxExtents.y, minExtents.z - 1000.0f, maxExtents.z + 1000.0f);

        cascades[i].zNear = nearDist;
        cascades[i].zFar = farDist;
        cascades[i].viewProj = lightProj * lightView;
    }
}

glm::mat4 CascadedShadowMap::getCascadeMatrix(int cascade) const {
    if (cascade >= 0 && cascade < NUM_CASCADES) {
        return cascades[cascade].viewProj;
    }
    return glm::mat4(1.0f);
}

GLuint CascadedShadowMap::getShadowMapTexture(int cascade) const {
    if (cascade >= 0 && cascade < NUM_CASCADES) {
        return shadowMapTextures[cascade];
    }
    return 0;
}

GLuint CascadedShadowMap::getFramebuffer(int cascade) const {
    if (cascade >= 0 && cascade < NUM_CASCADES) {
        return shadowMapFramebuffers[cascade];
    }
    return 0;
}

void CascadedShadowMap::bindForWriting(int cascade) const {
    if (cascade < 0 || cascade >= NUM_CASCADES) return;
    glBindFramebuffer(GL_FRAMEBUFFER, shadowMapFramebuffers[cascade]);
    glViewport(0, 0, SHADOW_MAP_RESOLUTION, SHADOW_MAP_RESOLUTION);
}

void CascadedShadowMap::bindForReading(int cascade, unsigned int textureUnit) const {
    if (cascade < 0 || cascade >= NUM_CASCADES) return;
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_2D, shadowMapTextures[cascade]);
}

CascadedShadowMap::CascadeInfo CascadedShadowMap::getCascadeInfo(int cascade) const {
    if (cascade >= 0 && cascade < NUM_CASCADES) {
        return cascades[cascade];
    }
    return CascadeInfo{zNear, zFar, glm::mat4(1.0f)};
}
