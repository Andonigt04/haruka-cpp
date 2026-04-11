#include "octree.h"
#include "physics_engine.h"
#include <iostream>
#include <algorithm>

namespace Haruka {

OctreeNode::OctreeNode(AABB bounds, int depth)
    : bounds(bounds), depth(depth) {
    for (int i = 0; i < 8; i++) {
        children[i] = nullptr;
    }
}

OctreeNode::~OctreeNode() {}

void OctreeNode::subdivide() {
    if (depth >= MAX_DEPTH) return;
    
    glm::dvec3 size = (bounds.max - bounds.min) / 2.0;
    glm::dvec3 center = bounds.min + size;
    
    // 8 octantes
    for (int i = 0; i < 8; i++) {
        glm::dvec3 min = bounds.min;
        glm::dvec3 max = bounds.min + size;
        
        if (i & 1) min.x += size.x;
        if (i & 2) min.y += size.y;
        if (i & 4) min.z += size.z;
        
        if (i & 1) max.x += size.x;
        if (i & 2) max.y += size.y;
        if (i & 4) max.z += size.z;
        
        children[i] = std::make_unique<OctreeNode>(AABB(min, max), depth + 1);
    }
}

int OctreeNode::getOctant(glm::dvec3 pos) const {
    glm::dvec3 center = (bounds.min + bounds.max) / 2.0;
    int octant = 0;
    if (pos.x > center.x) octant |= 1;
    if (pos.y > center.y) octant |= 2;
    if (pos.z > center.z) octant |= 4;
    return octant;
}

void OctreeNode::insert(std::shared_ptr<RigidBody> body) {
    if (!bounds.contains(body->position)) return;
    
    if (isLeaf() && bodies.size() < MAX_BODIES) {
        bodies.push_back(body);
    } else {
        if (isLeaf()) subdivide();
        
        if (!isLeaf()) {
            int octant = getOctant(body->position);
            children[octant]->insert(body);
        } else {
            bodies.push_back(body);
        }
    }
}

void OctreeNode::remove(std::shared_ptr<RigidBody> body) {
    if (!bounds.contains(body->position)) return;
    
    auto it = std::find(bodies.begin(), bodies.end(), body);
    if (it != bodies.end()) {
        bodies.erase(it);
        return;
    }
    
    if (!isLeaf()) {
        int octant = getOctant(body->position);
        children[octant]->remove(body);
    }
}

void OctreeNode::getCollidingBodies(const AABB& region, std::vector<std::shared_ptr<RigidBody>>& result) {
    if (!bounds.intersects(region)) return;
    
    for (auto& body : bodies) {
        result.push_back(body);
    }
    
    if (!isLeaf()) {
        for (int i = 0; i < 8; i++) {
            if (children[i]) {
                children[i]->getCollidingBodies(region, result);
            }
        }
    }
}

Octree::Octree(glm::dvec3 center, double size) {
    glm::dvec3 min = center - glm::dvec3(size / 2.0);
    glm::dvec3 max = center + glm::dvec3(size / 2.0);
    root = std::make_unique<OctreeNode>(AABB(min, max));
}

Octree::~Octree() {}

void Octree::insert(std::shared_ptr<RigidBody> body) {
    root->insert(body);
}

void Octree::remove(std::shared_ptr<RigidBody> body) {
    root->remove(body);
}

void Octree::getNearbodies(std::shared_ptr<RigidBody> body, std::vector<std::shared_ptr<RigidBody>>& result) {
    AABB region(body->position - glm::dvec3(body->radius * 2.0),
                body->position + glm::dvec3(body->radius * 2.0));
    root->getCollidingBodies(region, result);
}

void Octree::rebuild() {
    // Reconstruir octree (útil después de actualizar muchos cuerpos)
}

}