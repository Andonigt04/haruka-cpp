#include "terrain_renderer.h"
#include <cstdio>

namespace Haruka {

TerrainRenderer::~TerrainRenderer() {
    for (auto& pair : m_gpuMeshes) {
        cleanupMesh(pair.second);
    }
}

void TerrainRenderer::addToScene(const std::string& planetName, const PlanetChunkKey& key, const ChunkData& data) {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    uint64_t hash = ChunkCache::keyToHash(key);

    // Si ya existe, no lo volvemos a crear
    if (m_gpuMeshes.count(hash)) return;

    RenderMesh mesh;
    mesh.indexCount  = (uint32_t)data.indices.size();
    mesh.vertexCount = (uint32_t)data.vertices.size();
    mesh.planetName  = planetName;
    mesh.chunkCenter = data.chunkCenter;
    mesh.terrainMode = static_cast<int>(data.terrainMode);
    mesh.lod         = data.key.lod;
    mesh.planetRadius = data.planetRadius;

    glGenVertexArrays(1, &mesh.vao);
    glBindVertexArray(mesh.vao);

    // Attr 0: positions (relative to chunkCenter)
    glGenBuffers(1, &mesh.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, data.vertices.size() * sizeof(glm::vec3), data.vertices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
    glEnableVertexAttribArray(0);

    // Attr 3: morph-target positions (CDLOD geomorphing)
    if (!data.morphTargets.empty()) {
        glGenBuffers(1, &mesh.mbo);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.mbo);
        glBufferData(GL_ARRAY_BUFFER, data.morphTargets.size() * sizeof(glm::vec3), data.morphTargets.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
        glEnableVertexAttribArray(3);
    }

    // Attr 1: normals
    if (!data.normals.empty()) {
        glGenBuffers(1, &mesh.nbo);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.nbo);
        glBufferData(GL_ARRAY_BUFFER, data.normals.size() * sizeof(glm::vec3), data.normals.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
        glEnableVertexAttribArray(1);
    }

    // Attr 2: UVs
    if (!data.uvs.empty()) {
        glGenBuffers(1, &mesh.uvo);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.uvo);
        glBufferData(GL_ARRAY_BUFFER, data.uvs.size() * sizeof(glm::vec2), data.uvs.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(glm::vec2), nullptr);
        glEnableVertexAttribArray(2);
    }

    // EBO
    glGenBuffers(1, &mesh.ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, data.indices.size() * sizeof(unsigned int), data.indices.data(), GL_STATIC_DRAW);

    mesh.isReady = true;
    m_gpuMeshes[hash] = mesh;

    glBindVertexArray(0);
}

void TerrainRenderer::removeFromScene(const std::string& planetName, const PlanetChunkKey& key) {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    uint64_t hash = ChunkCache::keyToHash(key);

    if (m_gpuMeshes.count(hash)) {
        cleanupMesh(m_gpuMeshes[hash]);
        m_gpuMeshes.erase(hash);
    }
}

void TerrainRenderer::render(const Haruka::WorldPos& cameraPos) {
    std::lock_guard<std::mutex> lock(m_renderMutex);

    GLint prog = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    GLint loc = glGetUniformLocation(prog, "u_chunkOffset");

    for (auto& [hash, mesh] : m_gpuMeshes) {
        if (!mesh.isReady) continue;
        if (loc >= 0) {
            glm::vec3 offset = glm::vec3(mesh.chunkCenter - glm::dvec3(cameraPos));
            glUniform3fv(loc, 1, &offset[0]);
        }
        glBindVertexArray(mesh.vao);
        glDrawElements(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(0);
}

void TerrainRenderer::renderPlanet(const std::string& planet, const Haruka::WorldPos& cameraPos) {
    std::lock_guard<std::mutex> lock(m_renderMutex);

    GLint prog = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    if (prog == 0) {
        fprintf(stderr, "[TerrainRenderer] ERROR: no shader bound — skipping terrain draw\n");
        return;
    }
    // layout(location=10) in planet.vert, layout(location=11) in planet.frag
    const GLint locOffset = 10;
    const GLint locMode   = 11;
    const GLint locMorph  = 12; // CDLOD morph factor (per-chunk)

    // Must match LODSystem(splitFactor=1.0). A leaf chunk of LOD L is visible in
    // the chunk-center-distance band [size*sf, 2*size*sf]; morph in its upper half.
    const double splitFactor = 1.0;

    for (auto& [hash, mesh] : m_gpuMeshes) {
        if (!mesh.isReady || mesh.planetName != planet) continue;

        // Distance to the chunk CENTRE (on the reference sphere → height-independent).
        glm::dvec3 toCenter = mesh.chunkCenter - glm::dvec3(cameraPos);
        glm::vec3  offset    = glm::vec3(toCenter);
        glUniform3fv(locOffset, 1, &offset[0]);
        glUniform1i(locMode, mesh.terrainMode);

        double dist     = glm::length(toCenter);
        double nodeSize = mesh.planetRadius * 2.0 / double(1u << mesh.lod);
        double low      = nodeSize * splitFactor;
        double high     = nodeSize * splitFactor * 2.0;
        double t        = (high > low) ? (dist - low) / (high - low) : 0.0;
        double morph    = (t - 0.5) / 0.5;            // morph in upper half of band
        morph = morph < 0.0 ? 0.0 : (morph > 1.0 ? 1.0 : morph);
        glUniform1f(locMorph, (float)morph);

        glBindVertexArray(mesh.vao);
        glDrawElements(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(0);
}

TerrainRenderer::DrawStats TerrainRenderer::getDrawStats() const {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    DrawStats s;
    for (const auto& [hash, mesh] : m_gpuMeshes) {
        if (!mesh.isReady) continue;
        s.draws++;
        s.vertices   += (int)mesh.vertexCount;
        s.triangles  += (int)(mesh.indexCount / 3);
    }
    return s;
}

TerrainRenderer::DrawStats TerrainRenderer::getDrawStatsForPlanet(const std::string& planet) const {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    DrawStats s;
    for (const auto& [hash, mesh] : m_gpuMeshes) {
        if (!mesh.isReady || mesh.planetName != planet) continue;
        s.draws++;
        s.vertices   += (int)mesh.vertexCount;
        s.triangles  += (int)(mesh.indexCount / 3);
    }
    return s;
}

void TerrainRenderer::cleanupMesh(RenderMesh& mesh) {
    if (mesh.vao) glDeleteVertexArrays(1, &mesh.vao);
    if (mesh.vbo) glDeleteBuffers(1, &mesh.vbo);
    if (mesh.nbo) glDeleteBuffers(1, &mesh.nbo);
    if (mesh.uvo) glDeleteBuffers(1, &mesh.uvo);
    if (mesh.ebo) glDeleteBuffers(1, &mesh.ebo);
}

}