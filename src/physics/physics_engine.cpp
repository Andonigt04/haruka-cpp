#include "physics_engine.h"

#include <iostream>
#include <algorithm>
#include <cmath>

PhysicsEngine::PhysicsEngine() {}

PhysicsEngine::~PhysicsEngine() {}

void PhysicsEngine::addBody(std::shared_ptr<RigidBody> body) {
    bodies.push_back(body);
    if (octree) octree->insert(body);
}

void PhysicsEngine::removeBody(const std::string& name) {
    auto it = std::find_if(bodies.begin(), bodies.end(),
        [&name](const std::shared_ptr<RigidBody>& b) { return b->name == name; });
    if (it != bodies.end()) {
        bodies.erase(it);
    }
}

std::shared_ptr<RigidBody> PhysicsEngine::getBody(const std::string& name) {
    auto it = std::find_if(bodies.begin(), bodies.end(),
        [&name](const std::shared_ptr<RigidBody>& b) { return b->name == name; });
    return (it != bodies.end()) ? *it : nullptr;
}

void PhysicsEngine::update(double deltaTime) {
    integrateForces(deltaTime);
    broadPhaseAABB();
    detectCollisions();
    resolveCollisions();
}

void PhysicsEngine::integrateForces(double dt) {
    for (auto& body : bodies) {
        if (body->isKinematic) continue;
        
        // Gravedad
        body->acceleration = gravity;
        
        // Velocity Verlet
        body->velocity += body->acceleration * dt;
        body->position += body->velocity * dt;
    }
}

void PhysicsEngine::detectCollisions() {
    collisions.clear();
    
    if (!octree) {
        // Fallback a all-pairs si no hay octree
        for (size_t i = 0; i < bodies.size(); i++) {
            for (size_t j = i + 1; j < bodies.size(); j++) {
                auto& a = bodies[i];
                auto& b = bodies[j];
                
                glm::dvec3 delta = b->position - a->position;
                double dist = glm::length(delta);
                double minDist = a->radius + b->radius;
                
                if (dist < minDist) {
                    CollisionInfo info;
                    info.bodyA = a.get();
                    info.bodyB = b.get();
                    info.penetration = minDist - dist;
                    info.normal = glm::normalize(delta);
                    collisions.push_back(info);
                }
            }
        }
    } else {
        // Usar octree para acelerar búsqueda
        for (auto& body : bodies) {
            std::vector<std::shared_ptr<RigidBody>> nearby;
            octree->getNearbodies(body, nearby);
            
            for (auto& other : nearby) {
                if (body.get() == other.get()) continue;
                
                glm::dvec3 delta = other->position - body->position;
                double dist = glm::length(delta);
                double minDist = body->radius + other->radius;
                
                if (dist < minDist) {
                    CollisionInfo info;
                    info.bodyA = body.get();
                    info.bodyB = other.get();
                    info.penetration = minDist - dist;
                    info.normal = glm::normalize(delta);
                    collisions.push_back(info);
                }
            }
        }
    }
}

void PhysicsEngine::resolveCollisions() {
    for (auto& collision : collisions) {
        if (collision.bodyA->isKinematic && collision.bodyB->isKinematic) continue;
        
        // Separación elástica simple
        double totalMass = collision.bodyA->mass + collision.bodyB->mass;
        double ratio = collision.bodyA->mass / totalMass;
        
        if (!collision.bodyA->isKinematic) {
            collision.bodyA->position -= collision.normal * collision.penetration * ratio;
        }
        if (!collision.bodyB->isKinematic) {
            collision.bodyB->position += collision.normal * collision.penetration * (1.0 - ratio);
        }
    }
}

void PhysicsEngine::broadPhaseAABB() {
    // Placeholder: en escala astronómica necesitarías BVH/Octree
    // Por ahora detectamos all-pairs (O(n²))
}

void PhysicsEngine::initPlanetaryPhysics(Haruka::WorldSystem* ws, Haruka::PlanetarySystem* ps, RaycastSimple* rs) {
    worldSystem = ws;
    planetarySystem = ps;
    raycastSystem = rs;
}

double PhysicsEngine::calculateGravityAtPosition(const glm::dvec3& worldPos, glm::dvec3& outGravityDir) {
    if (!worldSystem) {
        outGravityDir = glm::dvec3(0.0, -1.0, 0.0);
        return 9.81;  // Default Earth gravity
    }
    
    glm::dvec3 totalGravity = glm::dvec3(0.0);
    
    // Sum gravity from all celestial bodies
    const auto& bodies = worldSystem->getBodies();
    for (const auto& body : bodies) {
        totalGravity += calculateGravityContribution(body, worldPos);
    }
    
    double magnitude = glm::length(totalGravity);
    if (magnitude > 1e-6) {
        outGravityDir = glm::normalize(totalGravity);
    } else {
        outGravityDir = glm::dvec3(0.0, -1.0, 0.0);
        magnitude = 0.0;
    }
    
    return magnitude;
}

glm::dvec3 PhysicsEngine::calculateGravityContribution(const Haruka::CelestialBody& body, const glm::dvec3& worldPos) {
    // Calculate vector from test position to body
    glm::dvec3 delta = body.worldPos - worldPos;
    double distance = glm::length(delta);
    
    // Avoid division by zero
    if (distance < 1e-6) {
        return glm::dvec3(0.0);
    }
    
    // Newton's universal law of gravitation: F = G * m1 * m2 / r²
    // Acceleration: a = F / m1 = G * m2 / r²
    double acceleration = gravitationalConstant * body.mass / (distance * distance);
    glm::dvec3 direction = glm::normalize(delta);
    
    return direction * acceleration;
}

bool PhysicsEngine::checkTerrainCollision(
    const glm::dvec3& worldPos,
    glm::dvec3* outCollisionPoint,
    glm::dvec3* outCollisionNormal) {
    
    if (!raycastSystem || !worldSystem) {
        return false;  // Cannot check collision without raycast system
    }
    
    // Calculate gravity direction at this position
    glm::dvec3 gravityDir;
    calculateGravityAtPosition(worldPos, gravityDir);
    
    // Raycast downward (opposite to gravity direction)
    glm::dvec3 rayDirection = -gravityDir;
    double rayDistance = maxCollisionRaycastDistanceKm * 1000.0;  // Convert km to world units
    
    // Perform raycast
    // This is a placeholder - actual implementation depends on RaycastSimple API
    // For now, we return false to indicate no collision
    
    return false;
}

void PhysicsEngine::applyGravity(const glm::dvec3& worldPos, double deltaTime, glm::dvec3& inOutVelocity) {
    glm::dvec3 gravityDir;
    double gravityMagnitude = calculateGravityAtPosition(worldPos, gravityDir);
    
    // Apply gravitational acceleration: v += a * dt
    glm::dvec3 gravityAcceleration = gravityDir * gravityMagnitude;
    inOutVelocity += gravityAcceleration * deltaTime;
}