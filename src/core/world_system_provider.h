/**
 * @file world_system_provider.h
 * @brief Adaptador CLIENTE de IWorldProvider sobre WorldSystem + PlanetarySystem.
 *
 * La física (HarukaPhysics) solo conoce `IWorldProvider` (sin GL). En el CLIENTE, el mundo real vive
 * en `WorldSystem` (cuerpos celestes, mar) y `PlanetarySystem` (altura del terreno). Este adaptador
 * traduce uno a otro. En el SERVIDOR (DGS) habrá otro adaptador sobre su estado autoritativo — misma
 * interfaz, sin este archivo (que sí depende del engine).
 */
#pragma once

#include "physics/world_provider.h"
#include "core/world_system.h"
#include "game/planetary_system.h"

namespace Haruka {

class WorldSystemProvider : public Physics::IWorldProvider {
public:
    WorldSystemProvider(WorldSystem* world, PlanetarySystem* planetary)
        : m_world(world), m_planetary(planetary) {}

    bool       hasActivePlanet()    const override { return m_world && m_world->hasActivePlanet(); }
    glm::dvec3 activePlanetCenter() const override { return m_world ? m_world->getActivePlanetCenter() : glm::dvec3(0.0); }
    double     activePlanetRadius() const override { return m_world ? m_world->getActivePlanetRadius() : 0.0; }

    const std::vector<Physics::GravBody>& gravBodies() const override {
        m_grav.clear();
        if (m_world) {
            const auto& bodies = m_world->getBodies();
            m_grav.reserve(bodies.size());
            for (const auto& b : bodies)
                m_grav.push_back({ b.worldPos, b.mass });   // WorldPos = dvec3, mismas unidades (sin conversión)
        }
        return m_grav;
    }

    double terrainHeightAt(const glm::dvec3& worldPos) const override {
        // Cliente: la altura sale de la MALLA (F10, vía sampleTerrainHeight). Metros sobre la esfera.
        return m_planetary ? m_planetary->sampleTerrainHeight(worldPos) : 0.0;
    }

private:
    WorldSystem*     m_world;
    PlanetarySystem* m_planetary;
    mutable std::vector<Physics::GravBody> m_grav;   // buffer reusado (gravBodies devuelve referencia)
};

} // namespace Haruka
