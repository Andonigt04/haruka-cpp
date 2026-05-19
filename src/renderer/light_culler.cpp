#include "light_culler.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

LightCuller::LightCuller() {}

LightCuller::Frustum LightCuller::extractFrustum(const glm::mat4& viewProj) {
    Frustum frustum;
    
    // Extract frustum planes from view-projection matrix
    const glm::mat4& m = viewProj;
    
    // Right plane
    frustum.planes[0] = glm::vec4(
        m[0][3] - m[0][0],
        m[1][3] - m[1][0],
        m[2][3] - m[2][0],
        m[3][3] - m[3][0]
    );
    
    // Left plane
    frustum.planes[1] = glm::vec4(
        m[0][3] + m[0][0],
        m[1][3] + m[1][0],
        m[2][3] + m[2][0],
        m[3][3] + m[3][0]
    );
    
    // Bottom plane
    frustum.planes[2] = glm::vec4(
        m[0][3] + m[0][1],
        m[1][3] + m[1][1],
        m[2][3] + m[2][1],
        m[3][3] + m[3][1]
    );
    
    // Top plane
    frustum.planes[3] = glm::vec4(
        m[0][3] - m[0][1],
        m[1][3] - m[1][1],
        m[2][3] - m[2][1],
        m[3][3] - m[3][1]
    );
    
    // Far plane
    frustum.planes[4] = glm::vec4(
        m[0][3] - m[0][2],
        m[1][3] - m[1][2],
        m[2][3] - m[2][2],
        m[3][3] - m[3][2]
    );
    
    // Near plane
    frustum.planes[5] = glm::vec4(
        m[0][3] + m[0][2],
        m[1][3] + m[1][2],
        m[2][3] + m[2][2],
        m[3][3] + m[3][2]
    );
    
    // Normalize planes
    for (int i = 0; i < 6; i++) {
        float len = glm::length(glm::vec3(frustum.planes[i]));
        frustum.planes[i] /= len;
    }
    
    return frustum;
}

bool LightCuller::isPointInFrustum(const glm::vec3& point, const Frustum& frustum) {
    for (int i = 0; i < 6; i++) {
        float dist = glm::dot(glm::vec3(frustum.planes[i]), point) + frustum.planes[i].w;
        if (dist < 0.0f) return false;
    }
    return true;
}

bool LightCuller::isSphereInFrustum(const glm::vec3& center, float radius, const Frustum& frustum) {
    for (int i = 0; i < 6; i++) {
        float dist = glm::dot(glm::vec3(frustum.planes[i]), center) + frustum.planes[i].w;
        if (dist < -radius) return false;
    }
    return true;
}

std::vector<LightCuller::CulledLight> LightCuller::cullLights(
    Haruka::SceneManager* scene,
    const glm::mat4& viewMatrix,
    const glm::mat4& projMatrix,
    int maxLights) {
    
    std::vector<CulledLight> result;
    result.reserve(maxLights);
    
    if (!scene) return result;
    
    // Extract frustum from view-projection matrix
    glm::mat4 viewProj = projMatrix * viewMatrix;
    Frustum frustum = extractFrustum(viewProj);
    
    // Recolectar todas las luces de la escena
    const auto& objects = scene->getAllObjects();
    std::vector<CulledLight> allLights;
    
    for (const auto& obj : objects) {
        if (!obj) continue;
        if (obj->type == "PointLight" || obj->type == "DirectionalLight" || obj->type == "Light") {
            CulledLight light;
            light.position = glm::vec3(obj->position);

            glm::vec3 color(1.0f);
            if (obj->properties.contains("color") && obj->properties["color"].is_array() && obj->properties["color"].size() >= 3) {
                color = glm::vec3(
                    obj->properties["color"][0].get<float>(),
                    obj->properties["color"][1].get<float>(),
                    obj->properties["color"][2].get<float>());
            }
            float intensity = obj->properties.value("intensity", 1.0f);
            light.color = color * intensity;
            
            // Calcular radio aproximado (30 unidades por defecto para point lights)
            light.radius = obj->properties.value("radius", 30.0f);
            
            allLights.push_back(light);
        }
    }
    
    totalLights = allLights.size();
    culledLights = 0;
    
    // Frustum culling - mantener solo luces visibles
    for (const auto& light : allLights) {
        // Para directional lights (distancia muy grande), siempre incluir
        float dist = glm::length(light.position);
        if (dist > 1000.0f) {
            // Directional light
            if (result.size() < maxLights) {
                result.push_back(light);
            }
        } else {
            // Point light - hacer frustum culling
            if (isSphereInFrustum(light.position, light.radius, frustum)) {
                if (result.size() < maxLights) {
                    result.push_back(light);
                }
            } else {
                culledLights++;
            }
        }
    }
    
    // Ordenar por distancia a la cámara (luces más cercanas primero)
    glm::vec3 camPos = glm::vec3(glm::inverse(viewMatrix)[3]);
    std::sort(result.begin(), result.end(), [camPos](const CulledLight& a, const CulledLight& b) {
        float distA = glm::length(a.position - camPos);
        float distB = glm::length(b.position - camPos);
        return distA < distB;
    });
    
    return result;
}
