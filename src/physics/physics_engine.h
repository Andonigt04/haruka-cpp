/**
 * @file physics_engine.h
 * @brief Rigid body physics engine with octree broad-phase and impulse-based collision resolution.
 */
#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <string>
#include "octree.h"
#include "core/world_system.h"
#include "game/planetary_system.h"

class RaycastSimple;

/** @brief Rigid body simulation record used by the physics engine. */
struct RigidBody {
    glm::dvec3 position;
    glm::dvec3 velocity;
    glm::dvec3 acceleration;
    double mass;
    double radius;
    bool isKinematic = false;
    std::string name;
};

/** @brief Collision resolution input/output data. */
struct CollisionInfo {
    RigidBody* bodyA;
    RigidBody* bodyB;
    double penetration;
    glm::dvec3 normal;
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
    
    /** @brief Advances simulation by one time step. */
    void update(double deltaTime);
    /** @brief Sets constant gravity acceleration. */
    void setGravity(glm::dvec3 g) { gravity = g; }
    /** @brief Returns current gravity acceleration. */
    glm::dvec3 getGravity() const { return gravity; }
    
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
    std::vector<CollisionInfo> collisions;
    glm::dvec3 gravity{0.0, -9.81, 0.0};
    std::unique_ptr<Octree> octree;
    
    // Planetary physics members
    Haruka::WorldSystem* worldSystem = nullptr;
    Haruka::PlanetarySystem* planetarySystem = nullptr;
    RaycastSimple* raycastSystem = nullptr;
    double gravitationalConstant = 6.67430e-11;  ///< Newton's gravitational constant
    double maxCollisionRaycastDistanceKm = 1000.0;  ///< Max distance to check for terrain collision
    
    /** @brief Integrates external forces for all bodies. */
    void integrateForces(double dt);
    /** @brief Detects collisions and fills collision list. */
    void detectCollisions();
    /** @brief Resolves collision responses for detected contacts. */
    void resolveCollisions();
    /** @brief Runs broad-phase AABB traversal/culling. */
    void broadPhaseAABB();
};

namespace Haruka {
using PhysicsEngine = ::PhysicsEngine;
}