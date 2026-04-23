#include "physics_engine.h"

#include <iostream>
#include <algorithm>
#include <cmath>


namespace Haruka {

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

}