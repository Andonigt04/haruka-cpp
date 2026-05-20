#include "world_system.h"
#include "game/planetary_system.h"
#include "core/scene/scene_manager.h"
#include "tools/object_types.h"

namespace Haruka {

    WorldSystem::WorldSystem() : m_worldOrigin(0.0, 0.0, 0.0) {}

    WorldSystem::~WorldSystem() = default;

    void WorldSystem::init() {
        m_planetarySystem = std::make_unique<PlanetarySystem>();
        m_planetarySystem->init();
        
        // Aquí podrías cargar tu base de datos de planetas inicial
    }

    void WorldSystem::update(double dt, const glm::dvec3& cameraPos) {
        // 1. GESTIÓN DE PRECISIÓN (Floating Origin)
        // Si la cámara se aleja más de 5000km del origen actual, reseteamos el origen.
        if (glm::length(cameraPos) > 5000.0) {
            shiftOrigin(cameraPos);
        }

        // 2. ACTUALIZAR SUBSISTEMAS
        // Pasamos la cámara y una matriz de vista-proyección (calculada con toLocal)
        m_planetarySystem->update(dt, cameraPos);
    }

    void WorldSystem::shiftOrigin(const glm::dvec3& newOrigin) {
        // Calculamos cuánto nos hemos movido
        glm::dvec3 offset = newOrigin - m_worldOrigin;
        
        // Actualizamos el origen global
        m_worldOrigin = newOrigin;

        // Movemos todos los objetos del universo para compensar
        for (auto& body : m_bodies) {
            body.worldPos -= offset;
        }
    }

    void WorldSystem::syncFromScene(const SceneManager& scene) {
        m_bodies.clear();
        for (const auto& objPtr : scene.getAllObjects()) {
            if (!objPtr) continue;
            ObjectType ot = stringToObjectType(objPtr->type);
            if (ot != ObjectType::STAR && !objPtr->flags.castLight) continue;

            CelestialBody body;
            body.name     = objPtr->name;
            body.type     = ObjectType::STAR;
            body.worldPos = glm::dvec3(objPtr->position);
            body.radius   = std::max({(float)objPtr->scale.x, (float)objPtr->scale.y, (float)objPtr->scale.z});
            body.mass     = 1.989e30; // default solar mass
            body.color    = glm::length(glm::vec3(objPtr->color)) > 0.001f
                            ? glm::vec3(objPtr->color)
                            : glm::vec3(1.0f, 0.95f, 0.85f);
            m_bodies.push_back(body);
        }
    }

    glm::vec3 WorldSystem::getDominantLightDirection(const glm::dvec3& observerPos) const {
        glm::dvec3 sum(0.0);
        for (const auto& body : m_bodies) {
            if (body.type != ObjectType::STAR) continue;
            glm::dvec3 delta = body.worldPos - observerPos;
            double dist2 = glm::dot(delta, delta);
            if (dist2 < 1e-12) continue;
            sum += delta / dist2; // direction weighted by 1/dist²
        }
        if (glm::length(sum) < 1e-9)
            return glm::vec3(0.0f, 1.0f, 0.0f);
        return glm::vec3(glm::normalize(sum));
    }

    glm::vec3 WorldSystem::getDominantLightColor(const glm::dvec3& observerPos) const {
        glm::dvec3 colorSum(0.0);
        double weightSum = 0.0;
        for (const auto& body : m_bodies) {
            if (body.type != ObjectType::STAR) continue;
            glm::dvec3 delta = body.worldPos - observerPos;
            double dist2 = glm::dot(delta, delta);
            if (dist2 < 1e-12) continue;
            double w = 1.0 / dist2;
            colorSum   += glm::dvec3(body.color) * w;
            weightSum  += w;
        }
        if (weightSum < 1e-12)
            return glm::vec3(1.0f);
        return glm::vec3(colorSum / weightSum);
    }

    glm::vec3 WorldSystem::toLocal(const glm::dvec3& worldPos) const {
        // Restamos la posición de la cámara en alta precisión (double)
        // y el resultado lo bajamos a float para la GPU.
        return glm::vec3(worldPos - m_worldOrigin);
    }
}