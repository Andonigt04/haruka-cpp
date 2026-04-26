#include "raycast_simple.h"

#include <glm/gtc/constants.hpp>
#include <iostream>

void RaycastSimple::rebuildTriangleCache() {
    triangles.clear();
    for (const auto& [_, tris] : meshTriangles) {
        triangles.insert(triangles.end(), tris.begin(), tris.end());
    }
}

void RaycastSimple::addMesh(const std::string& id,
                            const std::vector<glm::vec3>& vertices,
                            const std::vector<unsigned int>& indices) {
    std::vector<RaycastTriangle> localTriangles;
    localTriangles.reserve(indices.size() / 3);

    // Construir triángulos a partir de vertices e indices
    for (size_t i = 0; i < indices.size(); i += 3) {
        if (i + 2 >= indices.size()) break;

        unsigned int i0 = indices[i];
        unsigned int i1 = indices[i + 1];
        unsigned int i2 = indices[i + 2];

        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
            continue;
        }

        glm::vec3 v0 = vertices[i0];
        glm::vec3 v1 = vertices[i1];
        glm::vec3 v2 = vertices[i2];

        // Calcular normal
        glm::vec3 edge1 = v1 - v0;
        glm::vec3 edge2 = v2 - v0;
        glm::vec3 normal = glm::normalize(glm::cross(edge1, edge2));

        localTriangles.push_back({v0, v1, v2, normal});
    }

    meshTriangles[id] = std::move(localTriangles);
    rebuildTriangleCache();
}

void RaycastSimple::removeMesh(const std::string& id) {
    auto it = meshTriangles.find(id);
    if (it == meshTriangles.end()) return;
    meshTriangles.erase(it);
    rebuildTriangleCache();
}

void RaycastSimple::clearMeshes() {
    meshTriangles.clear();
    triangles.clear();
}

bool RaycastSimple::rayTriangleIntersect(const glm::vec3& rayOrigin,
                                         const glm::vec3& rayDir,
                                         const RaycastTriangle& tri,
                                         float& distance,
                                         glm::vec3& hitPoint) {
    const float EPSILON = 1e-8f;

    glm::vec3 edge1 = tri.v1 - tri.v0;
    glm::vec3 edge2 = tri.v2 - tri.v0;
    glm::vec3 h = glm::cross(rayDir, edge2);
    float a = glm::dot(edge1, h);

    if (glm::abs(a) < EPSILON) {
        return false;
    }

    float f = 1.0f / a;
    glm::vec3 s = rayOrigin - tri.v0;
    float u = f * glm::dot(s, h);

    if (u < 0.0f || u > 1.0f) {
        return false;
    }

    glm::vec3 q = glm::cross(s, edge1);
    float v = f * glm::dot(rayDir, q);

    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }

    float t = f * glm::dot(edge2, q);

    if (t > EPSILON) {
        distance = t;
        hitPoint = rayOrigin + rayDir * t;
        return true;
    }

    return false;
}

RaycastHit RaycastSimple::raycast(const glm::vec3& origin,
                                  const glm::vec3& direction,
                                  float maxDistance) {
    RaycastHit hit;
    float minDistance = maxDistance;
    int hitTriangleIndex = -1;

    glm::vec3 normalizedDir = glm::normalize(direction);

    for (int i = 0; i < (int)triangles.size(); i++) {
        float distance = 0.0f;
        glm::vec3 hitPoint = glm::vec3(0.0f);

        if (rayTriangleIntersect(origin, normalizedDir, triangles[i], distance, hitPoint)) {
            if (distance < minDistance && distance > 0.0f) {
                minDistance = distance;
                hit.position = hitPoint;
                hit.normal = triangles[i].normal;
                hit.distance = distance;
                hit.triangleIndex = i;
                hitTriangleIndex = i;
            }
        }
    }

    hit.hit = (hitTriangleIndex >= 0);
    return hit;
}
