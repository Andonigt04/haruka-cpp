#include <cmath>
#include "water_renderer.h"
#include <cstdio>

namespace Haruka {

WaterRenderer::~WaterRenderer() {
    for (auto& pair : m_gpuMeshes) cleanupMesh(pair.second);
}

void WaterRenderer::addToScene(const std::string& planetName, const PlanetChunkKey& key, const ChunkData& data) {
    if (!data.hasOcean || data.waterVertices.empty() || data.waterIndices.empty()) return;

    std::lock_guard<std::mutex> lock(m_renderMutex);
    uint64_t hash = ChunkCache::keyToHash(key);
    if (m_gpuMeshes.count(hash)) return;

    RenderMesh mesh;
    mesh.indexCount  = (uint32_t)data.waterIndices.size();
    mesh.vertexCount = (uint32_t)data.waterVertices.size();
    mesh.planetName  = planetName;
    mesh.chunkCenter = data.chunkCenter;
    {
        double nodeSz = data.planetRadius * 2.0 / double(1u << key.lod);
        mesh.cullRadius = (float)(nodeSz * 0.9);
    }

    glGenVertexArrays(1, &mesh.vao);
    glBindVertexArray(mesh.vao);

    glGenBuffers(1, &mesh.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, data.waterVertices.size() * sizeof(glm::vec3), data.waterVertices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
    glEnableVertexAttribArray(0);

    if (!data.waterNormals.empty()) {
        glGenBuffers(1, &mesh.nbo);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.nbo);
        glBufferData(GL_ARRAY_BUFFER, data.waterNormals.size() * sizeof(glm::vec3), data.waterNormals.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
        glEnableVertexAttribArray(1);
    }

    // Atributo 2: nivel de agua por vértice (0=océano, >0=lago). Si no viene
    // (malla v1), el atributo queda deshabilitado → el shader lo lee como 0 = océano.
    if (!data.waterParams.empty()) {
        glGenBuffers(1, &mesh.pbo);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.pbo);
        glBufferData(GL_ARRAY_BUFFER, data.waterParams.size() * sizeof(glm::vec2), data.waterParams.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(glm::vec2), nullptr); // x=nivel, y=profundidad
        glEnableVertexAttribArray(2);
    }

    glGenBuffers(1, &mesh.ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, data.waterIndices.size() * sizeof(unsigned int), data.waterIndices.data(), GL_STATIC_DRAW);

    mesh.isReady = true;
    m_gpuMeshes[hash] = mesh;
    glBindVertexArray(0);
}

void WaterRenderer::removeFromScene(const std::string& planetName, const PlanetChunkKey& key) {
    (void)planetName;
    std::lock_guard<std::mutex> lock(m_renderMutex);
    uint64_t hash = ChunkCache::keyToHash(key);
    auto it = m_gpuMeshes.find(hash);
    if (it != m_gpuMeshes.end()) {
        cleanupMesh(it->second);
        m_gpuMeshes.erase(it);
    }
}

void WaterRenderer::renderPlanet(const std::string& planet, const Haruka::WorldPos& cameraPos) {
    std::lock_guard<std::mutex> lock(m_renderMutex);

    GLint prog = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    if (prog == 0) return;

    const GLint locOffset = 10; // u_chunkOffset (shared convention with terrain)

    glm::vec4 planes[6];
    if (m_cullEnabled) {
        const glm::mat4& m = m_cullVP;
        for (int i = 0; i < 4; ++i) planes[0][i] = m[i][3] + m[i][0];
        for (int i = 0; i < 4; ++i) planes[1][i] = m[i][3] - m[i][0];
        for (int i = 0; i < 4; ++i) planes[2][i] = m[i][3] + m[i][1];
        for (int i = 0; i < 4; ++i) planes[3][i] = m[i][3] - m[i][1];
        for (int i = 0; i < 4; ++i) planes[4][i] = m[i][3] + m[i][2];
        for (int i = 0; i < 4; ++i) planes[5][i] = m[i][3] - m[i][2];
        for (int p = 0; p < 6; ++p) planes[p] /= glm::length(glm::vec3(planes[p]));
    }

    // Two-sided: the ocean shell must be visible from BELOW too (underwater) — with
    // back-face culling (inherited from the terrain pass) you'd see nothing once the
    // camera dips under the surface. Restore the cull state after.
    GLboolean cullWas = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_CULL_FACE);

    // Horizon-cull setup (see TerrainRenderer).
    glm::dvec3 camFromCenter = glm::dvec3(cameraPos) - m_planetCenter;
    double     camDist       = glm::length(camFromCenter);
    glm::dvec3 camDir        = camDist > 1.0 ? camFromCenter / camDist : glm::dvec3(0,1,0);
    const double occR        = m_planetRadius - 15000.0;
    const bool   horizonOn   = m_hasPlanetCenter && occR > 1.0 && camDist > occR + 2000.0;

    for (auto& [hash, mesh] : m_gpuMeshes) {
        if (!mesh.isReady || mesh.planetName != planet) continue;
        glm::vec3 offset = glm::vec3(mesh.chunkCenter - glm::dvec3(cameraPos));

        if (horizonOn) {
            glm::dvec3 chunkFromCenter = mesh.chunkCenter - m_planetCenter;
            double cl = glm::length(chunkFromCenter);
            if (cl > 1e-6) {
                double cosChunk    = glm::dot(camDir, chunkFromCenter / cl);
                double cosHorizon  = occR / camDist;
                double horizonDist = std::sqrt(camDist*camDist - occR*occR);
                double chunkDist   = glm::length(glm::dvec3(offset));
                if (cosChunk < cosHorizon && chunkDist > horizonDist) continue;
            }
        }

        if (m_cullEnabled) {
            bool inside = true;
            for (int p = 0; p < 6; ++p) {
                float d = planes[p].x*offset.x + planes[p].y*offset.y + planes[p].z*offset.z + planes[p].w;
                if (d < -mesh.cullRadius) { inside = false; break; }
            }
            if (!inside) continue;
        }

        glUniform3fv(locOffset, 1, &offset[0]);
        glBindVertexArray(mesh.vao);
        glDrawElements(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(0);
    if (cullWas) glEnable(GL_CULL_FACE);
}

void WaterRenderer::cleanupMesh(RenderMesh& mesh) {
    if (mesh.vao) glDeleteVertexArrays(1, &mesh.vao);
    if (mesh.vbo) glDeleteBuffers(1, &mesh.vbo);
    if (mesh.nbo) glDeleteBuffers(1, &mesh.nbo);
    if (mesh.pbo) glDeleteBuffers(1, &mesh.pbo);
    if (mesh.ebo) glDeleteBuffers(1, &mesh.ebo);
}

}
