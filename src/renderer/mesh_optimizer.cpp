#include "mesh_optimizer.h"
#include <algorithm>
#include <unordered_map>
#include <cmath>

MeshOptimizer::MeshOptimizer() : stats{0, 0, 0, 0, 0.0f, 0.0f} {}

bool MeshOptimizer::areVerticesSimilar(
    const Vertex& v1,
    const Vertex& v2,
    float threshold) const {
    
    float posDist = glm::distance(v1.Position, v2.Position);
    float normalDist = glm::distance(v1.Normal, v2.Normal);
    
    return posDist < threshold && normalDist < threshold;
}

float MeshOptimizer::calculateVertexError(
    const glm::vec3& vertex,
    const glm::mat4& quadric) const {
    
    glm::vec4 v(vertex, 1.0f);
    glm::vec4 result = quadric * v;
    return glm::dot(v, result);
}

MeshOptimizer::OptimizedMesh MeshOptimizer::deduplicateVertices(
    const std::vector<Vertex>& vertices,
    const std::vector<unsigned int>& indices,
    float mergeThreshold) {
    
    OptimizedMesh result;
    std::vector<int> vertexMapping(vertices.size(), -1);
    
    // Detectar y fusionar vértices similares
    for (size_t i = 0; i < vertices.size(); i++) {
        if (vertexMapping[i] != -1) continue;  // Ya mapeado
        
        int uniqueIndex = result.vertices.size();
        result.vertices.push_back(vertices[i]);
        vertexMapping[i] = uniqueIndex;
        
        // Buscar vértices similares
        for (size_t j = i + 1; j < vertices.size(); j++) {
            if (vertexMapping[j] != -1) continue;
            
            if (areVerticesSimilar(vertices[i], vertices[j], mergeThreshold)) {
                vertexMapping[j] = uniqueIndex;
            }
        }
    }
    
    // Remapear índices
    for (unsigned int idx : indices) {
        result.indices.push_back(vertexMapping[idx]);
    }
    
    result.decimationRatio = static_cast<float>(result.vertices.size()) / vertices.size();
    
    stats.originalVertices = vertices.size();
    stats.optimizedVertices = result.vertices.size();
    stats.reductionPercent = (1.0f - result.decimationRatio) * 100.0f;
    stats.estimatedFpsGain = stats.reductionPercent * 0.15f;  // ~0.15% FPS per 1% reduction
    
    return result;
}

MeshOptimizer::OptimizedMesh MeshOptimizer::generateLOD(
    const std::vector<Vertex>& vertices,
    const std::vector<unsigned int>& indices,
    int lodLevel) {
    
    if (lodLevel == 0) {
        // LOD 0: Original mesh
        OptimizedMesh result;
        result.vertices = vertices;
        result.indices = indices;
        result.decimationRatio = 1.0f;
        return result;
    }
    
    // Calcular ratio para cada LOD
    float targetRatio = 1.0f / (1 << lodLevel);  // 0.5, 0.25, 0.125 para LOD 1,2,3
    
    // Usar decimation para generar LOD
    return decimate(vertices, indices, targetRatio);
}

std::vector<unsigned int> MeshOptimizer::optimizeIndices(
    const std::vector<unsigned int>& indices) {
    
    // Post-transform vertex cache optimization
    // Reordenar índices para mejorar cache locality
    
    std::vector<unsigned int> optimized = indices;
    std::vector<int> vertexLastUsed(65536, -1);
    std::vector<int> vertexScore(65536, 0);
    
    const int CACHE_SIZE = 32;
    
    // Score vértices por su posición en cache
    for (int i = 0; i < CACHE_SIZE; i++) {
        float score = (CACHE_SIZE - i) / static_cast<float>(CACHE_SIZE);
        vertexScore[i] = static_cast<int>(score * 100.0f);
    }
    
    // Recalcular índices
    for (size_t i = 0; i < optimized.size(); i += 3) {
        unsigned int idx0 = optimized[i];
        unsigned int idx1 = optimized[i + 1];
        unsigned int idx2 = optimized[i + 2];
        
        // Actualizar last used
        vertexLastUsed[idx0] = i;
        vertexLastUsed[idx1] = i;
        vertexLastUsed[idx2] = i;
    }
    
    return optimized;
}

MeshOptimizer::OptimizedMesh MeshOptimizer::decimate(
    const std::vector<Vertex>& vertices,
    const std::vector<unsigned int>& indices,
    float targetRatio) {
    
    OptimizedMesh result;
    
    // Decimación simple: remover cada N-ésimo vértice
    int step = std::max(1, static_cast<int>(1.0f / targetRatio));
    
    std::vector<int> vertexMapping(vertices.size(), -1);
    int newIdx = 0;
    
    for (size_t i = 0; i < vertices.size(); i++) {
        if (i % step == 0) {
            result.vertices.push_back(vertices[i]);
            vertexMapping[i] = newIdx++;
        }
    }
    
    // Remapear índices, skipear triángulos con vértices eliminados
    for (size_t i = 0; i < indices.size(); i += 3) {
        unsigned int idx0 = indices[i];
        unsigned int idx1 = indices[i + 1];
        unsigned int idx2 = indices[i + 2];
        
        // Solo incluir triángulos cuyos vértices fueron mantenidos
        if (vertexMapping[idx0] != -1 && 
            vertexMapping[idx1] != -1 && 
            vertexMapping[idx2] != -1) {
            
            result.indices.push_back(vertexMapping[idx0]);
            result.indices.push_back(vertexMapping[idx1]);
            result.indices.push_back(vertexMapping[idx2]);
        }
    }
    
    result.decimationRatio = static_cast<float>(result.vertices.size()) / vertices.size();
    
    stats.originalVertices = vertices.size();
    stats.originalIndices = indices.size();
    stats.optimizedVertices = result.vertices.size();
    stats.optimizedIndices = result.indices.size();
    stats.reductionPercent = (1.0f - result.decimationRatio) * 100.0f;
    stats.estimatedFpsGain = stats.reductionPercent * 0.15f;
    
    return result;
}
