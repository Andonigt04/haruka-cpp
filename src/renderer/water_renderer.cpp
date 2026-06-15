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
    // Igual que el terreno: si ya existe lo saltamos, SALVO que esté "stale" → lo
    // reemplazamos en el sitio (sin agujero). Si no existía pero estaba marcado
    // stale (raro), limpiamos la marca.
    bool stale = m_stale.count(hash) != 0;
    if (m_gpuMeshes.count(hash)) {
        if (!stale) return;
        cleanupMesh(m_gpuMeshes[hash]);
        m_stale.erase(hash);
    } else if (stale) {
        m_stale.erase(hash);
    }

    RenderMesh mesh;
    mesh.indexCount  = (uint32_t)data.waterIndices.size();
    mesh.vertexCount = (uint32_t)data.waterVertices.size();
    mesh.planetName  = planetName;
    mesh.key         = key;
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
    // El agua NO se autopurga: su borrado lo dirige el TerrainRenderer (callback
    // onChunkRemoved) para que agua y terreno cubran SIEMPRE lo mismo (sin huecos
    // de océano ni mallas de agua huérfanas). Aquí solo reemplazamos si era stale.
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
    m_stale.erase(hash);
}

bool WaterRenderer::isResident(const PlanetChunkKey& key) const {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    auto it = m_gpuMeshes.find(ChunkCache::keyToHash(key));
    return it != m_gpuMeshes.end() && it->second.isReady;
}

void WaterRenderer::markStale(const PlanetChunkKey& key) {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    uint64_t hash = ChunkCache::keyToHash(key);
    if (m_gpuMeshes.count(hash)) m_stale.insert(hash);
}

void WaterRenderer::purgeStaleCoveredBy(const PlanetChunkKey& key) {
    // CALLER HOLDS m_renderMutex. Misma lógica que TerrainRenderer.
    // (1) SUBDIVIDE: padre stale + 4 hijos residentes no-stale → fuera el padre.
    if (key.lod > 0) {
        PlanetChunkKey parent{ key.face, (uint8_t)(key.lod - 1), key.x >> 1, key.y >> 1 };
        uint64_t ph = ChunkCache::keyToHash(parent);
        if (m_stale.count(ph) && m_gpuMeshes.count(ph)) {
            const uint8_t  cl = (uint8_t)(parent.lod + 1);
            const uint32_t bx = parent.x * 2, by = parent.y * 2;
            const PlanetChunkKey kids[4] = {
                { parent.face, cl, bx,     by     }, { parent.face, cl, bx + 1, by     },
                { parent.face, cl, bx,     by + 1 }, { parent.face, cl, bx + 1, by + 1 },
            };
            bool allReady = true;
            for (const auto& kk : kids) {
                uint64_t kh = ChunkCache::keyToHash(kk);
                auto it = m_gpuMeshes.find(kh);
                // Nota: un hijo de agua puede no existir si esa celda no toca océano;
                // solo exigimos que los que existan no estén stale.
                if (it != m_gpuMeshes.end() && (!it->second.isReady || m_stale.count(kh))) { allReady = false; break; }
            }
            if (allReady) { cleanupMesh(m_gpuMeshes[ph]); m_gpuMeshes.erase(ph); m_stale.erase(ph); }
        }
    }
    // (2) MERGE: 'key' grueso cubre cualquier agua stale más fina de su área → fuera.
    std::vector<uint64_t> drop;
    for (auto& [h, mesh] : m_gpuMeshes) {
        if (!m_stale.count(h)) continue;
        const PlanetChunkKey& s = mesh.key;
        if (s.face == key.face && s.lod > key.lod) {
            uint32_t shift = (uint32_t)(s.lod - key.lod);
            if ((s.x >> shift) == key.x && (s.y >> shift) == key.y) drop.push_back(h);
        }
    }
    for (uint64_t h : drop) { cleanupMesh(m_gpuMeshes[h]); m_gpuMeshes.erase(h); m_stale.erase(h); }
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
        // chunkCenter es PLANET-LOCAL → sumamos el centro ACTUAL (cuerpos móviles).
        glm::dvec3 worldChunkCenter = mesh.chunkCenter + m_planetCenter;
        glm::vec3 offset = glm::vec3(worldChunkCenter - glm::dvec3(cameraPos));

        if (horizonOn) {
            glm::dvec3 chunkFromCenter = mesh.chunkCenter; // ya relativo al centro
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

} // namespace Haruka
