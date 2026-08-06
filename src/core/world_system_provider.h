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
#include "core/terrain/cube_sphere.h"

#include <unordered_map>
#include <mutex>
#include <cmath>

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
        // LA SUPERFICIE DE REFERENCIA, la misma que leen props, scripts y el render (ver
        // PlanetarySystem::sampleTerrainHeight). La física NO puede tener su propio suelo: cuando lo
        // tuvo —analítico puntual sobre una retícula de 8 m inventada— caminabas sobre una superficie
        // que no era la dibujada (7.81 m de pico con meso) y los props flotaban.
        if (!m_planetary || !hasActivePlanet()) return 0.0;
        return m_planetary->sampleTerrainHeight(worldPos);
    }

    // (Fase 2b) Malla LOCAL del terreno para la colisión de Jolt: se muestrea la altura de la MALLA en
    // una rejilla TANGENTE alrededor de `center` (parche localmente plano sobre la esfera). Sigue el
    // terreno REAL (mismo `sampleTerrainHeight` que la altura) sin tener que iterar el set de chunks.
    bool terrainMesh(const glm::dvec3& center, double radius,
                     std::vector<glm::dvec3>& outVerts, std::vector<uint32_t>& outTris) const override {
        if (!m_world || !m_planetary || !m_world->hasActivePlanet()) return false;
        const glm::dvec3 pc = m_world->getActivePlanetCenter();
        const double     R  = m_world->getActivePlanetRadius();
        const glm::dvec3 rel = center - pc;
        const double d = glm::length(rel);
        if (d < 1e-6 || R <= 0.0) return false;
        const glm::dvec3 up = rel / d;
        glm::dvec3 t1 = glm::normalize(glm::cross(up, std::abs(up.y) < 0.99 ? glm::dvec3(0,1,0) : glm::dvec3(1,0,0)));
        const glm::dvec3 t2 = glm::cross(up, t1);
        const int    N    = 32;                          // rejilla N×N (≈ 2·radius/N de paso ≈ 6 m con r=96)
        const double step = (2.0 * radius) / (N - 1);
        // La malla de colisión se muestrea de la SUPERFICIE DE REFERENCIA (reference_surface.h): la
        // retícula de vértices del LOD de referencia, que es exactamente la que dibuja el render
        // alrededor del jugador. Ni la malla dibujada (dependía del draw set → al girar la cámara los
        // chunks bajo los pies salían del frustum, el draw set colapsaba al ancestro grueso y el suelo
        // saltaba cientos de metros → CAÍAS) ni el analítico puntual (detalle infinito que ningún
        // vértice representa). Barata: caché de nudos que no caduca y parches que se solapan ~75% al
        // andar. Lleva el DEFORM aplicado y es la misma fuente que puede evaluar el servidor.
        Haruka::WorldGenParams W; double Rp;
        if (!m_planetary->getActivePlanetParams(W, Rp)) return false;
        outVerts.clear(); outVerts.reserve((size_t)N * N);
        for (int j = 0; j < N; ++j) for (int i = 0; i < N; ++i) {
            const double x = -radius + i * step, z = -radius + j * step;
            const glm::dvec3 dir = glm::normalize(rel + t1 * x + t2 * z);     // dirección desde el planeta
            const double h = m_planetary->sampleTerrainHeight(pc + dir * R);  // barato (caché) + deform
            outVerts.push_back(pc + dir * (R + h));                           // punto de superficie (mundo)
        }
        outTris.clear(); outTris.reserve((size_t)(N - 1) * (N - 1) * 6);
        for (int j = 0; j < N - 1; ++j) for (int i = 0; i < N - 1; ++i) {
            const uint32_t a = j * N + i, b = a + 1, c = a + N, dd = c + 1;
            outTris.insert(outTris.end(), { a, b, c, b, dd, c });   // winding → normal saliente (arriba)
        }
        return true;
    }

private:
    // Sin caché ni retícula PROPIAS: las tenía y ese era el bug. La altura la define
    // `PlanetarySystem::sampleTerrainHeight` → `ReferenceSurface`, que ya cachea por nudo de retícula.
    WorldSystem*     m_world;
    PlanetarySystem* m_planetary;
    mutable std::vector<Physics::GravBody> m_grav;   // buffer reusado (gravBodies devuelve referencia)
};

} // namespace Haruka
