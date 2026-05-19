#include "mesh_lod.h"
#include <glad/glad.h>
#include <algorithm>

MeshLOD::MeshLOD() {}

MeshLOD::~MeshLOD() {
    for (auto& lod : lodLevels) {
        if (lod.VAO) glDeleteVertexArrays(1, &lod.VAO);
        if (lod.VBO) glDeleteBuffers(1, &lod.VBO);
        if (lod.EBO) glDeleteBuffers(1, &lod.EBO);
    }
}

void MeshLOD::setupGL(LODLevel& level) {
    glGenVertexArrays(1, &level.VAO);
    glGenBuffers(1, &level.VBO);
    glGenBuffers(1, &level.EBO);

    glBindVertexArray(level.VAO);
    
    glBindBuffer(GL_ARRAY_BUFFER, level.VBO);
    glBufferData(GL_ARRAY_BUFFER, level.vertices.size() * sizeof(Vertex), 
                 level.vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, level.EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, level.indices.size() * sizeof(unsigned int),
                 level.indices.data(), GL_STATIC_DRAW);

    // Vertex attributes
    glEnableVertexAttribArray(0);  // Position
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), 
                         (void*)offsetof(Vertex, Position));

    glEnableVertexAttribArray(1);  // Normal
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                         (void*)offsetof(Vertex, Normal));

    glEnableVertexAttribArray(2);  // TexCoord
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                         (void*)offsetof(Vertex, TexCoords));

    glBindVertexArray(0);
}

void MeshLOD::generateLODs(
    const std::vector<Vertex>& vertices,
    const std::vector<unsigned int>& indices,
    const std::vector<float>& distances) {
    
    lodLevels.clear();

    // LOD 0: Original mesh (distancia 0-distances[0])
    {
        LODLevel lod0;
        lod0.vertices = vertices;
        lod0.indices = indices;
        lod0.minDistance = 0.0f;
        lod0.maxDistance = distances.size() > 0 ? distances[0] : 10.0f;
        setupGL(lod0);
        lodLevels.push_back(lod0);
    }

    // LOD 1, 2, 3: Versiones decimadas
    for (size_t i = 1; i < 4; i++) {
        if (i - 1 >= distances.size()) break;

        auto optimized = optimizer.generateLOD(vertices, indices, i);
        
        LODLevel lod;
        lod.vertices = optimized.vertices;
        lod.indices = optimized.indices;
        lod.minDistance = distances[i - 1];
        lod.maxDistance = (i < distances.size()) ? distances[i] : 1000.0f;
        setupGL(lod);
        lodLevels.push_back(lod);
    }
}

int MeshLOD::selectLOD(float distance) const {
    for (size_t i = 0; i < lodLevels.size(); i++) {
        if (distance >= lodLevels[i].minDistance && 
            distance < lodLevels[i].maxDistance) {
            return i;
        }
    }
    return lodLevels.size() - 1;  // Último LOD para distancias muy lejanas
}

void MeshLOD::render(float distance) {
    if (lodLevels.empty()) return;

    int lodIdx = selectLOD(distance);
    const auto& lod = lodLevels[lodIdx];

    glBindVertexArray(lod.VAO);
    glDrawElements(GL_TRIANGLES, lod.indices.size(), GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}

MeshLOD::LODStats MeshLOD::getStats() const {
    LODStats stats{};
    stats.totalLODLevels = lodLevels.size();

    for (const auto& lod : lodLevels) {
        stats.totalVertices += lod.vertices.size();
        stats.totalIndices += lod.indices.size();
    }

    // Aproximar reducción de memoria
    if (lodLevels.size() > 0) {
        int originalSize = lodLevels[0].vertices.size() * lodLevels[0].indices.size();
        int optimizedSize = stats.totalVertices * stats.totalIndices;
        if (originalSize > 0) {
            stats.memoryReduction = 1.0f - (static_cast<float>(optimizedSize) / originalSize);
        }
    }

    return stats;
}
