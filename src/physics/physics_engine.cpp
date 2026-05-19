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
    resolveStaticCollisions();
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
    for (auto& col : collisions) {
        bool aKin = col.bodyA->isKinematic;
        bool bKin = col.bodyB->isKinematic;
        if (aKin && bKin) continue;

        if (aKin) {
            // bodyA is static — bodyB absorbs all correction
            col.bodyB->position += col.normal * col.penetration;
            double vn = glm::dot(col.bodyB->velocity, col.normal);
            if (vn < 0.0) col.bodyB->velocity -= col.normal * vn;
        } else if (bKin) {
            // bodyB is static — bodyA absorbs all correction
            col.bodyA->position -= col.normal * col.penetration;
            double vn = glm::dot(col.bodyA->velocity, -col.normal);
            if (vn < 0.0) col.bodyA->velocity += col.normal * vn;
        } else {
            // Both dynamic — split by mass
            double totalMass = col.bodyA->mass + col.bodyB->mass;
            double ratioA = col.bodyB->mass / totalMass;
            double ratioB = col.bodyA->mass / totalMass;
            col.bodyA->position -= col.normal * col.penetration * ratioA;
            col.bodyB->position += col.normal * col.penetration * ratioB;
        }
    }
}

void PhysicsEngine::addStaticBox(const glm::dvec3& center, const glm::dvec3& halfExtents) {
    staticBoxes.push_back({ center - halfExtents, center + halfExtents });
}

void PhysicsEngine::clearStaticBoxes() {
    staticBoxes.clear();
}

void PhysicsEngine::broadPhaseAABB() {
    if (!octree || bodies.empty()) return;
    // Rebuild the octree each step so moved bodies are found correctly.
    // Each body is re-inserted using its updated position from integrateForces().
    for (auto& body : bodies) {
        octree->remove(body);
        octree->insert(body);
    }
}

void PhysicsEngine::resolveStaticCollisions() {
    for (auto& body : bodies) {
        if (body->isKinematic) continue;

        for (const auto& box : staticBoxes) {
            // Sphere center (body->position IS the sphere center)
            const glm::dvec3& c = body->position;
            const double      r = body->radius;

            // Closest point on AABB to sphere center
            glm::dvec3 closest = glm::clamp(c, box.bmin, box.bmax);
            glm::dvec3 diff    = c - closest;
            double dist = glm::length(diff);

            double penetration = r - dist;
            if (penetration <= 0.0) continue;

            glm::dvec3 normal;
            if (dist < 1e-9) {
                // Center is inside the box — push out on the minimum-penetration axis
                double px = std::min(c.x - box.bmin.x, box.bmax.x - c.x);
                double py = std::min(c.y - box.bmin.y, box.bmax.y - c.y);
                double pz = std::min(c.z - box.bmin.z, box.bmax.z - c.z);
                if (px <= py && px <= pz)
                    normal = (c.x < (box.bmin.x + box.bmax.x) * 0.5) ? glm::dvec3(-1,0,0) : glm::dvec3(1,0,0);
                else if (py <= px && py <= pz)
                    normal = (c.y < (box.bmin.y + box.bmax.y) * 0.5) ? glm::dvec3(0,-1,0) : glm::dvec3(0,1,0);
                else
                    normal = (c.z < (box.bmin.z + box.bmax.z) * 0.5) ? glm::dvec3(0,0,-1) : glm::dvec3(0,0,1);
                penetration = r + std::min({px, py, pz});
            } else {
                normal = diff / dist;
            }

            // Push body out of the box
            body->position += normal * penetration;

            // Cancel velocity component directed into the surface
            double vn = glm::dot(body->velocity, normal);
            if (vn < 0.0) body->velocity -= normal * vn;
        }
    }
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