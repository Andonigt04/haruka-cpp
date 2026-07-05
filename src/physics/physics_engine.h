/**
 * @file physics_engine.h
 * @brief Rigid body physics engine with octree broad-phase and impulse-based collision resolution.
 */
#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include "octree.h"
#include "core/world_system.h"
#include "game/planetary_system.h"

namespace Haruka { namespace Physics {

class RaycastSimple;

/** @brief Rigid body simulation record used by the physics engine. */
struct RigidBody {
    glm::dvec3 position;
    glm::dvec3 velocity;
    glm::dvec3 acceleration;
    double mass;
    double radius;
    bool isKinematic = false;
    bool inWater = false;   // estado agua↔aire para disparar el splash SOLO al ENTRAR (no cada frame)
    std::string name;
};

/** @brief Collision resolution input/output data. */
struct CollisionInfo {
    RigidBody* bodyA;
    RigidBody* bodyB;
    double penetration;
    glm::dvec3 normal;
};

/** @brief Axis-aligned static box for scene geometry collision. */
struct StaticBox {
    glm::dvec3 bmin;
    glm::dvec3 bmax;
};

/** @brief Caja ORIENTADA estática (objetos colocados: mesas, props…). center/halfExtents
 *  en mundo + rot (local→mundo, ortonormal). Caja ajustada al modelo, orientada a la
 *  superficie. */
struct StaticOBB {
    glm::dvec3 center;
    glm::dvec3 halfExtents;
    glm::dmat3 rot;
};

/**
 * @brief Main physics simulation coordinator.
 *
 * Owns rigid body storage, broad-phase acceleration structure, and collision state.
 */
class PhysicsEngine {
public:
    /** @brief Constructs an empty physics engine. */
    PhysicsEngine();
    ~PhysicsEngine();
    
    /** @brief Adds a body to the simulation. */
    void addBody(std::shared_ptr<RigidBody> body);
    /** @brief Removes body by name. */
    void removeBody(const std::string& name);
    /** @brief Returns body by name or null when missing. */
    std::shared_ptr<RigidBody> getBody(const std::string& name);

    /** @brief Registers a static AABB for scene geometry collision. */
    void addStaticBox(const glm::dvec3& center, const glm::dvec3& halfExtents);
    /** @brief Removes all static boxes (e.g. on scene reload). */
    void clearStaticBoxes();

    /** @brief Registra/limpia cajas orientadas de objetos colocados (mesas, estaciones). */
    void addPlacedOBB(const glm::dvec3& center, const glm::dvec3& halfExtents, const glm::dmat3& rot);
    // Obstáculos colocados (paredes/estructuras) — para navegación/propagación (sonido, conjuros).
    const std::vector<StaticOBB>& getPlacedOBBs() const { return placedOBBs; }
    void clearPlacedOBBs();

    /** @brief Cajas de los RECURSOS del mundo (árboles/rocas) — lista aparte porque se
     *  regeneran al moverse, independiente de los objetos colocados. */
    void addPropOBB(const glm::dvec3& center, const glm::dvec3& halfExtents, const glm::dmat3& rot);
    void clearPropOBBs();
    /** @brief Empuja una esfera fuera de los OBB colocados (te subes encima o te frena).
     *  Devuelve el centro corregido; pone grounded=true si el empuje fue a favor de 'up'. */
    glm::dvec3 resolveSphere(const glm::dvec3& center, double radius,
                             const glm::dvec3& up, bool& grounded) const;

    /** @brief Advances simulation by one time step. */
    void update(double deltaTime);
    /** @brief Sets constant gravity acceleration. */
    /** @brief Callback de SPLASH: se dispara cuando un cuerpo dinámico CRUZA la superficie del mar
     *  hacia dentro con velocidad de entrada apreciable. (pos superficie, velocidad de impacto,
     *  radio). El juego lo engancha para emitir partículas PBF → acopla rígidos↔fluido. */
    void setWaterEntryCallback(std::function<void(const glm::dvec3&, const glm::dvec3&, double)> cb) {
        m_onWaterEntry = std::move(cb);
    }

    void setGravity(glm::dvec3 g) { gravity = g; }
    /** @brief Returns current gravity acceleration. */
    glm::dvec3 getGravity() const { return gravity; }

    /** @brief Sets the ambient WIND velocity (m/s) used for aerodynamic drag. The
     *  atmosphere (WorldSystem) provides it; the engine applies it per active body
     *  in integrateForces (O(bodies), inherentemente localizado — sin coste global). */
    void setWind(const glm::dvec3& windVel) { m_wind = windVel; }
    
    /** @brief Returns collision events from last update. */
    const std::vector<CollisionInfo>& getCollisions() const { return collisions; }
    /** @brief Replaces broad-phase octree root. */
    void initOctree(glm::dvec3 center, double size) {
        octree = std::make_unique<Octree>(center, size);
    }
    
    /**
     * @brief Initializes planetary physics systems.
     * @param worldSystem World system for body information
     * @param planetarySystem Planetary system for gravity calculation
     * @param raycastSystem Physics raycast system for collisions
     */
    void initPlanetaryPhysics(Haruka::WorldSystem* worldSystem, Haruka::PlanetarySystem* planetarySystem, RaycastSimple* raycastSystem);
    
    /**
     * @brief Calculates gravitational acceleration at a world position.
     * 
     * Sums gravitational contributions from all celestial bodies in the world.
     * @param worldPos Position in world space
     * @param outGravityDir Output direction of gravity (normalized)
     * @return Gravitational acceleration magnitude (m/s²)
     */
    double calculateGravityAtPosition(const glm::dvec3& worldPos, glm::dvec3& outGravityDir);
    
    /**
     * @brief Calculates gravity contribution from a single celestial body.
     * @param body Celestial body to calculate gravity from
     * @param worldPos Test position in world space
     * @return Gravitational acceleration vector (m/s²)
     */
    glm::dvec3 calculateGravityContribution(const Haruka::CelestialBody& body, const glm::dvec3& worldPos);
    
    /**
     * @brief Checks if a position collides with terrain.
     * @param worldPos Position to check for collision
     * @param outCollisionPoint Optional: world position of collision point
     * @param outCollisionNormal Optional: surface normal at collision point
     * @return true if collision detected, false otherwise
     */
    bool checkTerrainCollision(
        const glm::dvec3& worldPos,
        glm::dvec3* outCollisionPoint = nullptr,
        glm::dvec3* outCollisionNormal = nullptr);
    
    /**
     * @brief Applies gravity force to an object.
     * @param worldPos Current position of the object
     * @param deltaTime Time step
     * @param inOutVelocity Velocity to update with gravity acceleration
     */
    void applyGravity(const glm::dvec3& worldPos, double deltaTime, glm::dvec3& inOutVelocity);
    
    /**
     * @brief Sets the gravitational constant.
     * @param G Gravitational constant to use
     */
    void setGravitationalConstant(double G) { gravitationalConstant = G; }
    
    double getGravitationalConstant() const { return gravitationalConstant; }
    
    /**
     * @brief Sets the maximum raycast distance for terrain collision detection.
     * @param maxDistanceKm Maximum raycast distance in km
     */
    void setCollisionRaycastDistance(double maxDistanceKm) { maxCollisionRaycastDistanceKm = maxDistanceKm; }

private:
    std::vector<std::shared_ptr<RigidBody>> bodies;
    std::vector<StaticBox>                  staticBoxes;
    std::vector<StaticOBB>                  placedOBBs;   // objetos colocados por el jugador
    std::vector<StaticOBB>                  propOBBs;     // recursos del mundo (árboles/rocas)
    std::vector<CollisionInfo> collisions;
    glm::dvec3 gravity{0.0, -9.81, 0.0};

    // Arrastre aerodinámico: viento ambiente (m/s) + coeficientes SUAVES (la
    // resistencia del aire amortigua hacia 0; el viento empuja sutilmente). Valores
    // pequeños para no zarandear al jugador; afecta sobre todo a objetos sueltos.
    std::function<void(const glm::dvec3&, const glm::dvec3&, double)> m_onWaterEntry; // splash al entrar al agua
    glm::dvec3 m_wind{0.0};
    double     m_airDamp  = 0.10;  // amortiguación del aire (1/s) hacia velocidad 0
    double     m_windCoef = 0.010; // acoplamiento cuadrático con la vel. relativa al viento

    std::unique_ptr<Octree> octree;

    // Planetary physics members
    Haruka::WorldSystem* worldSystem = nullptr;
    Haruka::PlanetarySystem* planetarySystem = nullptr;
    RaycastSimple* raycastSystem = nullptr;
    double gravitationalConstant = 6.67430e-11;
    double maxCollisionRaycastDistanceKm = 1000.0;

    /** @brief Integrates external forces for all bodies. */
    void integrateForces(double dt);
    /** @brief Detects collisions and fills collision list. */
    void detectCollisions();
    /** @brief Resolves collision responses for detected contacts. */
    void resolveCollisions();
    /** @brief Resolves sphere vs static AABB contacts. */
    void resolveStaticCollisions();
    /** @brief Runs broad-phase AABB traversal/culling. */
    void broadPhaseAABB();
};

}} // namespace Haruka::Physics

// Back-compat aliases during the namespace migration.
using Haruka::Physics::RigidBody;
using Haruka::Physics::CollisionInfo;
using Haruka::Physics::StaticBox;
using Haruka::Physics::StaticOBB;
using Haruka::Physics::PhysicsEngine;
namespace Haruka {
    using Physics::RigidBody; using Physics::CollisionInfo; using Physics::StaticBox;
    using Physics::StaticOBB; using Physics::PhysicsEngine;
}