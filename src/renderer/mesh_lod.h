/**
 * @file mesh_lod.h
 * @brief Mesh LOD manager — generates and selects simplified variants by camera distance.
 */
#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <glad/glad.h>
#include "mesh_optimizer.h"

/**
 * @brief Mesh level-of-detail manager.
 *
 * Generates and renders simplified mesh variants based on camera distance.
 */
class MeshLOD {
public:
    /** @brief Constructs an empty LOD manager. */
    MeshLOD();
    /** @brief Releases generated LOD resources. */
    ~MeshLOD();

    /**
     * @brief Generates LOD levels from source mesh buffers.
     * @param vertices Source vertices.
     * @param indices Source indices.
     * @param distances Split distances per LOD level.
     */
    void generateLODs(
        const std::vector<Vertex>& vertices,
        const std::vector<unsigned int>& indices,
        const std::vector<float>& distances = {10.0f, 30.0f, 100.0f}
    );

    /** @brief Returns the best LOD index for a given distance. */
    int selectLOD(float distance) const;

    /** @brief Renders the mesh using the selected LOD level. */
    void render(float distance);

    /** @brief Statistics for generated LOD levels. */
    struct LODStats {
        int totalLODLevels;
        int totalVertices;
        int totalIndices;
        float memoryReduction;
    };

    /** @brief Returns current LOD statistics. */
    LODStats getStats() const;

private:
    struct LODLevel {
        GLuint VAO = 0, VBO = 0, EBO = 0;
        std::vector<Vertex> vertices;
        std::vector<unsigned int> indices;
        float minDistance = 0.0f, maxDistance = 0.0f;
    };
    std::vector<LODLevel> lodLevels;
    MeshOptimizer optimizer;
    /** @brief Allocates OpenGL buffers for one LOD level. */
    void setupGL(LODLevel& level);
};

