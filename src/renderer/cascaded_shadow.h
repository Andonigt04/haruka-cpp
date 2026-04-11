#pragma once

#include <glm/glm.hpp>
#include <glad/glad.h>
#include <vector>

/**
 * @brief Cascaded directional shadow map system.
 *
 * Provides four cascades with PCF-friendly distribution and smooth transitions.
 */

class CascadedShadowMap {
public:
    static constexpr int NUM_CASCADES = 4;
    static constexpr int SHADOW_MAP_RESOLUTION = 2048;

    /** @brief Constructs an uninitialized cascaded shadow system. */
    CascadedShadowMap();
    ~CascadedShadowMap();

    /**
     * @brief Initializes cascade split parameters and GPU resources.
     * @param zNear Near plane.
     * @param zFar Far plane.
     * @param lambda Cascade distribution factor.
     */
    void init(float zNear, float zFar, float lambda = 0.5f);

    /** @brief Recomputes cascade matrices from camera and light state. */
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

    /** @brief Returns the current cascade view-projection matrix. */
    glm::mat4 getCascadeMatrix(int cascade) const;

    /** @brief Returns shadow texture id for one cascade. */
    GLuint getShadowMapTexture(int cascade) const;
    /** @brief Returns framebuffer id for one cascade. */
    GLuint getFramebuffer(int cascade) const;
    /** @brief Binds one cascade for depth-only rendering. */
    void bindForWriting(int cascade) const;
    /** @brief Binds one cascade shadow map for sampling. */
    void bindForReading(int cascade, unsigned int textureUnit) const;

    /** @brief Per-cascade split and matrix info. */
    struct CascadeInfo {
        float zNear;
        float zFar;
        glm::mat4 viewProj;
    };

    /** @brief Returns one cascade info record. */
    CascadeInfo getCascadeInfo(int cascade) const;

    /** @brief Returns number of cascades. */
    int getNumCascades() const { return NUM_CASCADES; }
    /** @brief Returns shadow map resolution per cascade. */
    int getShadowMapResolution() const { return SHADOW_MAP_RESOLUTION; }

private:
    std::vector<GLuint> shadowMapTextures;
    std::vector<GLuint> shadowMapFramebuffers;
    std::vector<CascadeInfo> cascades;

    float zNear, zFar, lambda;

    /** @brief Allocates resources for one cascade. */
    void createShadowMap(int cascade);
};
