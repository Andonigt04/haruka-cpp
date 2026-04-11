#pragma once

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtx/intersect.hpp>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief Lightweight ray-triangle raycasting utility.
 *
 * Intended for terrain height queries and simple collision tests with minimal overhead.
 */

/** @brief Raycast hit result. */
struct RaycastHit {
    bool hit = false;
    float distance = 0.0f;
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 normal = glm::vec3(0.0f, 1.0f, 0.0f);
    int triangleIndex = -1;
};

/** @brief Triangle data cached for ray intersection tests. */
struct RaycastTriangle {
    glm::vec3 v0, v1, v2;
    glm::vec3 normal;
};

/** @brief Simple mesh raycaster for terrain/scene queries. */
class RaycastSimple {
public:
    /** @brief Constructs an empty raycaster. */
    RaycastSimple() = default;
    /** @brief Releases raycaster resources. */
    ~RaycastSimple() = default;

    /**
     * @brief Adds one mesh to the raycast acceleration set.
     */
    void addMesh(const std::string& id, 
                 const std::vector<glm::vec3>& vertices,
                 const std::vector<unsigned int>& indices);

    /** @brief Removes one mesh from raycast set by id. */
    void removeMesh(const std::string& id);

    /** @brief Clears all registered meshes. */
    void clearMeshes();

    /** @brief Casts a ray and returns the closest hit. */
    RaycastHit raycast(const glm::vec3& origin, 
                       const glm::vec3& direction,
                       float maxDistance = 1000.0f);

    /** @brief Convenience downward raycast for ground-height queries. */
    RaycastHit raycastDown(const glm::vec3& position, float maxDistance = 1000.0f) {
        return raycast(position, glm::vec3(0.0f, -1.0f, 0.0f), maxDistance);
    }

private:
    std::unordered_map<std::string, std::vector<RaycastTriangle>> meshTriangles;
    std::vector<RaycastTriangle> triangles;

    /** @brief Rebuilds flattened triangle list after mesh map updates. */
    void rebuildTriangleCache();

    /** @brief Tests one ray against one triangle. */
    bool rayTriangleIntersect(const glm::vec3& rayOrigin,
                              const glm::vec3& rayDir,
                              const RaycastTriangle& tri,
                              float& distance,
                              glm::vec3& hitPoint);
};
