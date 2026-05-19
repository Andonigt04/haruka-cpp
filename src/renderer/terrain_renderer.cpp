#include "terrain_renderer.h"

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

    // 1. Crear VAO
    glGenVertexArrays(1, &mesh.vao);
    glBindVertexArray(mesh.vao);

    // 2. VBO para Posiciones (Atributo 0)
    glGenBuffers(1, &mesh.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, data.vertices.size() * sizeof(glm::vec3), data.vertices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
    glEnableVertexAttribArray(0);

    // 3. NBO para Normales (Atributo 1)
    if (!data.normals.empty()) {
        glGenBuffers(1, &mesh.nbo);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.nbo);
        glBufferData(GL_ARRAY_BUFFER, data.normals.size() * sizeof(glm::vec3), data.normals.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
        glEnableVertexAttribArray(1);
    }

    // 4. EBO para Índices
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

    // Aquí deberías activar tu Shader de Planeta antes de este bucle
    for (auto& [hash, mesh] : m_gpuMeshes) {
        if (!mesh.isReady) continue;

        glBindVertexArray(mesh.vao);
        glDrawElements(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, 0);
    }
    glBindVertexArray(0);
}

void TerrainRenderer::renderPlanet(const std::string& planet) {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    for (auto& [hash, mesh] : m_gpuMeshes) {
        if (!mesh.isReady || mesh.planetName != planet) continue;
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
    if (mesh.ebo) glDeleteBuffers(1, &mesh.ebo);
}

}