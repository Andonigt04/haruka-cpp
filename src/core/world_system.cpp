#include "world_system.h"

namespace Haruka {

    WorldSystem::WorldSystem() : m_worldOrigin(0.0, 0.0, 0.0) {}

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

    glm::vec3 WorldSystem::toLocal(const glm::dvec3& worldPos) const {
        // Restamos la posición de la cámara en alta precisión (double)
        // y el resultado lo bajamos a float para la GPU.
        return glm::vec3(worldPos - m_worldOrigin);
    }
}