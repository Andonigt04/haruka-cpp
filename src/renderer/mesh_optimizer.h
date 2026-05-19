/**
 * @file mesh_optimizer.h
 * @brief Mesh optimization utilities — LOD generation, vertex deduplication, decimation.
 */
#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include "mesh.h"

/**
 * @brief Mesh optimization utilities for LOD and index/vertex cleanup.
 */
class MeshOptimizer {
public:
    /** @brief Output of a mesh optimization pass. */
    struct OptimizedMesh {
        std::vector<Vertex> vertices;
        std::vector<unsigned int> indices;
        float decimationRatio;  // Reduction ratio (0.0-1.0)
    };

    /** @brief Constructs a mesh optimizer with zeroed stats. */
    MeshOptimizer();
    ~MeshOptimizer() = default;

    /**
     * @brief Generates an LOD variant for a mesh.
     * @param vertices Source vertices.
     * @param indices Source indices.
     * @param lodLevel LOD index (0=original, higher = simpler).
     * @return Optimized mesh buffers.
     */
    OptimizedMesh generateLOD(
        const std::vector<Vertex>& vertices,
        const std::vector<unsigned int>& indices,
        int lodLevel
    );

    /** @brief Reorders indices for better post-transform cache locality. */
    std::vector<unsigned int> optimizeIndices(
        const std::vector<unsigned int>& indices
    );

    /** @brief Deduplicates nearby vertices using a merge threshold. */
    OptimizedMesh deduplicateVertices(
        const std::vector<Vertex>& vertices,
        const std::vector<unsigned int>& indices,
        float mergeThreshold = 0.001f
    );

    /** @brief Performs simple decimation with a target triangle ratio. */
    OptimizedMesh decimate(
        const std::vector<Vertex>& vertices,
        const std::vector<unsigned int>& indices,
        float targetRatio  // 0.5 = 50% de los triángulos originales
    );

    /** @brief Mesh optimization statistics snapshot. */
    struct Stats {
        int originalVertices;
        int originalIndices;
        int optimizedVertices;
        int optimizedIndices;
        float reductionPercent;
        float estimatedFpsGain;
    };

    /** @brief Returns the last optimization statistics snapshot. */
    Stats getStats() const { return stats; }

private:
    struct Vertex_internal {
        glm::vec3 pos;
        glm::vec3 normal;
        glm::vec2 uv;
        int originalIndex;
    };

    // Quadric error metrics
    struct QEM {
        glm::mat4 Q;  // 4x4 matriz de error cuadrático
    };

    /** @brief Tests whether two vertices are close enough to merge. */
    bool areVerticesSimilar(
        const Vertex& v1,
        const Vertex& v2,
        float threshold
    ) const;

    /** @brief Estimates the error of placing a vertex under a quadric. */
    float calculateVertexError(
        const glm::vec3& vertex,
        const glm::mat4& quadric
    ) const;

    Stats stats;
};
