#ifndef OCTREE_H
#define OCTREE_H

#include <glm/glm.hpp>
#include <vector>
#include <memory>

namespace Haruka {

/** @brief Axis-aligned bounding box used by octree nodes. */
struct AABB {
    glm::dvec3 min;
    glm::dvec3 max;
    
    AABB() : min(0), max(0) {}
    AABB(glm::dvec3 m, glm::dvec3 M) : min(m), max(M) {}
    
    /** @brief Returns true when the point lies inside the bounds. */
    bool contains(glm::dvec3 point) const {
        return point.x >= min.x && point.x <= max.x &&
               point.y >= min.y && point.y <= max.y &&
               point.z >= min.z && point.z <= max.z;
    }
    
    /** @brief Returns true when two bounds overlap. */
    bool intersects(const AABB& other) const {
        return !(max.x < other.min.x || min.x > other.max.x ||
                 max.y < other.min.y || min.y > other.max.y ||
                 max.z < other.min.z || min.z > other.max.z);
    }
};

struct RigidBody;

/** @brief One node of the collision broad-phase octree. */
class OctreeNode {
public:
    /** @brief Constructs a node at the given bounds and depth. */
    OctreeNode(AABB bounds, int depth = 0);
    /** @brief Releases node resources recursively. */
    ~OctreeNode();
    
    /** @brief Inserts a body into this node or its children. */
    void insert(std::shared_ptr<RigidBody> body);
    /** @brief Removes a body from this subtree. */
    void remove(std::shared_ptr<RigidBody> body);
    /** @brief Collects bodies colliding with the query region. */
    void getCollidingBodies(const AABB& region, std::vector<std::shared_ptr<RigidBody>>& result);
    
    bool isLeaf() const { return children[0] == nullptr; }
    int getDepth() const { return depth; }
    const AABB& getBounds() const { return bounds; }

private:
    AABB bounds;
    int depth;
    std::vector<std::shared_ptr<RigidBody>> bodies;
    std::unique_ptr<OctreeNode> children[8];
    
    static const int MAX_BODIES = 4;
    static const int MAX_DEPTH = 10;
    
    /** @brief Splits node into eight children. */
    void subdivide();
    /** @brief Returns the octant index for a position. */
    int getOctant(glm::dvec3 pos) const;
};

/** @brief Octree wrapper used for broad-phase physics queries. */
class Octree {
public:
    /** @brief Constructs a root octree from center and size. */
    Octree(glm::dvec3 center, double size);
    /** @brief Releases octree resources. */
    ~Octree();
    
    /** @brief Inserts a body into the tree. */
    void insert(std::shared_ptr<RigidBody> body);
    /** @brief Removes a body from the tree. */
    void remove(std::shared_ptr<RigidBody> body);
    /** @brief Collects nearby bodies around the query body. */
    void getNearbodies(std::shared_ptr<RigidBody> body, std::vector<std::shared_ptr<RigidBody>>& result);
    /** @brief Rebuilds the octree from current state. */
    void rebuild();
    
    const OctreeNode* getRoot() const { return root.get(); }

private:
    std::unique_ptr<OctreeNode> root;
};

}

#endif