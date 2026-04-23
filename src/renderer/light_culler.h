#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <glm/glm.hpp>

#include "core/scene.h"

/**
 * @brief Dynamic light culling helper.
 *
 * Reduces the number of lights submitted to the GPU based on
 * frustum, distance, and range checks.
 */
class LightCuller {
public:
    /** @brief Frustum-visible light record used by the renderer. */
    struct CulledLight {
        glm::vec3 position;
        glm::vec3 color;
        float radius;  // Para point lights
    };

    /** @brief Constructs a light culling helper. */
    LightCuller();
    ~LightCuller();

    /**
     * @brief Culls scene lights for the current camera/frustum.
     * @param scene Scene containing light-emitting objects.
     * @param viewMatrix View matrix.
     * @param projMatrix Projection matrix.
     * @param maxLights Maximum number of lights to return.
     * @return Filtered light set ready for GPU upload.
     */
    std::vector<CulledLight> cullLights(
        Haruka::Scene* scene,
        const glm::mat4& viewMatrix,
        const glm::mat4& projMatrix,
        int maxLights = 256
    );

    /** @brief Total number of lights examined during last cull. */
    int getTotalLights() const { return totalLights; }
    /** @brief Number of lights rejected during last cull. */
    int getCulledLights() const { return culledLights; }

private:
    /** @brief Frustum plane set extracted from view-projection matrix. */
    struct Frustum {
        glm::vec4 planes[6];  // left, right, top, bottom, near, far
    };

    /** @brief Extracts frustum planes from combined view-projection matrix. */
    Frustum extractFrustum(const glm::mat4& viewProj);
    /** @brief Tests a point against the current frustum. */
    bool isPointInFrustum(const glm::vec3& point, const Frustum& frustum);
    /** @brief Tests a sphere against the current frustum. */
    bool isSphereInFrustum(const glm::vec3& center, float radius, const Frustum& frustum);

    // Vulkan buffer para luces visibles
    VkBuffer lightBuffer = VK_NULL_HANDLE;
    VkDeviceMemory lightBufferMemory = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    size_t maxLightsBuffer = 256;

    void createLightBuffer(VkDevice device, VkPhysicalDevice physicalDevice, size_t maxLights = 256);
    void destroyLightBuffer();
    void uploadLightsToBuffer(const std::vector<CulledLight>& lights);

    int totalLights = 0;
    int culledLights = 0;

    size_t lightCount = 0;
    // TODO: Implementar lógica de creación y destrucción de buffers y recursos Vulkan
};
