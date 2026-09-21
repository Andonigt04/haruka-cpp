#pragma once
/**
 * @file world/vox/vox_system.h
 * @brief EL CAMPO VOLUMETRICO Y LA FISICA, como modulo: (1) las paredes de las cuevas que el render
 *        remallo este frame van a Jolt con la MISMA malla (por revision: un chunk quieto no cuesta),
 *        y (2) el suelo cercano dibujado desde la geometria de la colision (`NearGroundRing`), que el
 *        pase v5 dejo apagado por defecto y sigue existiendo como salida de emergencia
 *        (`HARUKA_NEAR_RING=1`). Vivia en `Application` (`syncVoxColliders`, `updateNearGroundRing`).
 */
#include <cstdint>
#include <unordered_map>
#include <glm/glm.hpp>

namespace Haruka { namespace Core { class Camera; } class PlanetarySystem; namespace Physics { class PhysicsEngine; } }

namespace Haruka::World {

class VoxSystem {
public:
    struct Frame {
        Core::Camera*           camera  = nullptr;
        PlanetarySystem*        planets = nullptr;
        Physics::PhysicsEngine* physics = nullptr;
    };
    /// Por frame, antes de dibujar: lo remallado por el render → cuerpos de Jolt (y lo soltado, fuera).
    void syncColliders(const Frame& f);
    /// Antes de dibujar el planeta: entrega el anillo de la fisica al planeta si hay uno nuevo.
    void updateNearGroundRing(const Frame& f);

private:
    std::unordered_map<uint64_t, uint64_t> m_colliderRev;   ///< chunk → revision que tiene Jolt
    uint64_t   m_ringRev = ~0ull;                            ///< revision del anillo entregada
    glm::dvec3 m_ringAnchor{0.0};
};

} // namespace Haruka::World
