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

    // Si ya existe: lo saltamos, SALVO que esté "stale" (deformado) → lo reemplazamos en el
    // sitio (limpiar la malla vieja y crear la nueva) sin que haya habido agujero entremedias.
    bool stale = m_stale.count(hash) != 0;
    if (m_gpuMeshes.count(hash)) {
        if (!stale) return;
        cleanupMesh(m_gpuMeshes[hash]);
        m_stale.erase(hash);
    } else if (stale) {
        m_stale.erase(hash);
    }

    RenderMesh mesh;
    mesh.indexCount  = (uint32_t)data.indices.size();
    mesh.vertexCount = (uint32_t)data.vertices.size();
    mesh.planetName  = planetName;
    mesh.key         = key;
    mesh.chunkCenter = data.chunkCenter;
    mesh.terrainMode = static_cast<int>(data.terrainMode);
    mesh.lod         = data.key.lod;
    mesh.planetRadius = data.planetRadius;

    // Real bounding sphere from the surface vertices (skip the skirt verts, which
    // hang far below and would bloat the radius). AABB centre + farthest corner.
    {
        const size_t surfCount = data.morphTargets.size() ? data.morphTargets.size()
                                                           : data.vertices.size();
        glm::vec3 mn( 1e30f), mx(-1e30f);
        for (size_t i = 0; i < surfCount && i < data.vertices.size(); ++i) {
            mn = glm::min(mn, data.vertices[i]);
            mx = glm::max(mx, data.vertices[i]);
        }
        if (mx.x >= mn.x) {
            mesh.bsCenter = (mn + mx) * 0.5f;
            mesh.bsRadius = glm::length(mx - mesh.bsCenter);
        }
    }

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

    // Retira mallas STALE cuya área ya cubre esta recién añadida (sin agujero).
    purgeStaleCoveredBy(key);

    glBindVertexArray(0);
}

void TerrainRenderer::removeFromScene(const std::string& planetName, const PlanetChunkKey& key) {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    uint64_t hash = ChunkCache::keyToHash(key);

    if (m_gpuMeshes.count(hash)) {
        cleanupMesh(m_gpuMeshes[hash]);
        m_gpuMeshes.erase(hash);
        if (m_onRemoved) m_onRemoved(key);   // espejo: quita también su agua
    }
    m_stale.erase(hash);
}

bool TerrainRenderer::isResident(const PlanetChunkKey& key) const {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    auto it = m_gpuMeshes.find(ChunkCache::keyToHash(key));
    return it != m_gpuMeshes.end() && it->second.isReady;
}

void TerrainRenderer::residentHashes(std::unordered_set<uint64_t>& out) const {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    out.clear();
    out.reserve(m_gpuMeshes.size());
    for (const auto& [h, mesh] : m_gpuMeshes)
        if (mesh.isReady) out.insert(h);
}

void TerrainRenderer::markStale(const PlanetChunkKey& key) {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    uint64_t hash = ChunkCache::keyToHash(key);
    if (m_gpuMeshes.count(hash)) m_stale.insert(hash); // sigue dibujándose hasta cubrirse
}

void TerrainRenderer::purgeStaleCoveredBy(const PlanetChunkKey& key) {
    // CALLER HOLDS m_renderMutex.
    // (1) SUBDIVIDE: 'key' es un hijo fino. Si su PADRE está stale y los 4 hijos ya
    //     están residentes y NO-stale, el padre ya está cubierto → fuera.
    if (key.lod > 0) {
        PlanetChunkKey parent{ key.face, (uint8_t)(key.lod - 1), key.x >> 1, key.y >> 1, key.body };
        uint64_t ph = ChunkCache::keyToHash(parent);
        if (m_stale.count(ph) && m_gpuMeshes.count(ph)) {
            const uint8_t  cl = (uint8_t)(parent.lod + 1);
            const uint32_t bx = parent.x * 2, by = parent.y * 2;
            const uint16_t bd = parent.body;
            const PlanetChunkKey kids[4] = {
                { parent.face, cl, bx,     by,     bd }, { parent.face, cl, bx + 1, by,     bd },
                { parent.face, cl, bx,     by + 1, bd }, { parent.face, cl, bx + 1, by + 1, bd },
            };
            bool allReady = true;
            for (const auto& kk : kids) {
                uint64_t kh = ChunkCache::keyToHash(kk);
                auto it = m_gpuMeshes.find(kh);
                if (it == m_gpuMeshes.end() || !it->second.isReady || m_stale.count(kh)) { allReady = false; break; }
            }
            if (allReady) {
                PlanetChunkKey pk = m_gpuMeshes[ph].key;
                cleanupMesh(m_gpuMeshes[ph]); m_gpuMeshes.erase(ph); m_stale.erase(ph);
                if (m_onRemoved) m_onRemoved(pk);   // espejo: quita su agua
            }
        }
    }
    // (2) MERGE: 'key' es grueso. Cualquier malla stale MÁS FINA dentro de su área ya
    //     queda cubierta por 'key' → fuera.
    std::vector<PlanetChunkKey> drop;
    for (auto& [h, mesh] : m_gpuMeshes) {
        if (!m_stale.count(h)) continue;
        const PlanetChunkKey& s = mesh.key;
        if (s.body == key.body && s.face == key.face && s.lod > key.lod) {
            uint32_t shift = (uint32_t)(s.lod - key.lod);
            if ((s.x >> shift) == key.x && (s.y >> shift) == key.y) drop.push_back(s);
        }
    }
    for (const auto& dk : drop) {
        uint64_t h = ChunkCache::keyToHash(dk);
        cleanupMesh(m_gpuMeshes[h]); m_gpuMeshes.erase(h); m_stale.erase(h);
        if (m_onRemoved) m_onRemoved(dk);   // espejo: quita su agua
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

    // Extract 6 frustum planes from the camera-relative VP (Gribb-Hartmann) once.
    glm::vec4 planes[6];
    if (m_cullEnabled) {
        const glm::mat4& m = m_cullVP;
        // row = m[col][row] in glm (column-major); build planes in world (cam-rel) space.
        for (int i = 0; i < 4; ++i) planes[0][i] = m[i][3] + m[i][0]; // left
        for (int i = 0; i < 4; ++i) planes[1][i] = m[i][3] - m[i][0]; // right
        for (int i = 0; i < 4; ++i) planes[2][i] = m[i][3] + m[i][1]; // bottom
        for (int i = 0; i < 4; ++i) planes[3][i] = m[i][3] - m[i][1]; // top
        for (int i = 0; i < 4; ++i) planes[4][i] = m[i][3] + m[i][2]; // near
        for (int i = 0; i < 4; ++i) planes[5][i] = m[i][3] - m[i][2]; // far
        for (int p = 0; p < 6; ++p) planes[p] /= glm::length(glm::vec3(planes[p]));
    }

    // Horizon-cull setup (occlusion-cone test, "3D Engine Design for Virtual
    // Globes", Cozzi/Ring). A chunk past the planet's curvature is hidden: it is
    // both beyond the horizon angle (cos = R/d) AND farther than the horizon
    // distance sqrt(d²−R²). R uses each chunk's planetRadius minus a margin so
    // tall relief near the limb is never wrongly culled.
    glm::dvec3 camFromCenter = glm::dvec3(cameraPos) - m_planetCenter;
    double     camDist       = glm::length(camFromCenter);
    glm::dvec3 camDir        = camDist > 1.0 ? camFromCenter / camDist : glm::dvec3(0,1,0);

    int drawn = 0;
    for (auto& [hash, mesh] : m_gpuMeshes) {
        if (!mesh.isReady || mesh.planetName != planet) continue;

        // chunkCenter es PLANET-LOCAL (relativo al centro del planeta) → sumamos el
        // centro ACTUAL del planeta para obtener su posición en el mundo. Así el
        // planeta puede MOVERSE (orbitar) sin regenerar chunks: solo cambia el centro.
        glm::dvec3 worldChunkCenter = mesh.chunkCenter + m_planetCenter;
        glm::dvec3 toCenter = worldChunkCenter - glm::dvec3(cameraPos);
        glm::vec3  offset    = glm::vec3(toCenter);

        // Horizon cull: hide chunks beyond the planet's curvature. occR uses the
        // chunk's planetRadius minus a generous margin for the tallest peaks
        // (~11 km) + slack. Only engages when the camera is clearly ABOVE that
        // shell — at/near ground level the horizon is metres away and the test
        // would wrongly cull nearby visible chunks.
        if (m_hasPlanetCenter) {
            double occR = mesh.planetRadius - 15000.0;
            if (occR > 1.0 && camDist > occR + 2000.0) {
                glm::dvec3 chunkFromCenter = mesh.chunkCenter; // ya es relativo al centro
                double cl = glm::length(chunkFromCenter);
                if (cl > 1e-6) {
                    double cosChunk    = glm::dot(camDir, chunkFromCenter / cl);
                    double cosHorizon  = occR / camDist;
                    double horizonDist = std::sqrt(camDist*camDist - occR*occR);
                    double chunkDist   = glm::length(toCenter);
                    if (cosChunk < cosHorizon && chunkDist > horizonDist) continue;
                }
            }
        }

        // Frustum cull against the REAL bounding sphere (centre at terrain
        // elevation, radius from actual vertices) so visible chunks are never
        // wrongly culled. Falls back to a node-size estimate if not computed.
        if (m_cullEnabled) {
            glm::vec3 c = offset + mesh.bsCenter;
            float radius = mesh.bsRadius > 0.0f
                         ? mesh.bsRadius * 1.1f
                         : (float)(mesh.planetRadius * 2.0 / double(1u << mesh.lod));
            bool inside = true;
            for (int p = 0; p < 6; ++p) {
                float d = planes[p].x*c.x + planes[p].y*c.y + planes[p].z*c.z + planes[p].w;
                if (d < -radius) { inside = false; break; }
            }
            if (!inside) continue;
        }

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
        ++drawn;
    }
    glBindVertexArray(0);
    m_lastDrawn = drawn;
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

std::vector<PlanetChunkKey> TerrainRenderer::invalidateSphere(const glm::dvec3& center, double radius) {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    std::vector<PlanetChunkKey> hit;
    for (auto it = m_gpuMeshes.begin(); it != m_gpuMeshes.end();) {
        RenderMesh& m = it->second;
        // Bounding sphere en MUNDO: chunkCenter es planet-local → + centro del planeta.
        glm::dvec3 c = m.chunkCenter + m_planetCenter + glm::dvec3(m.bsCenter);
        double r = (m.bsRadius > 0.0f) ? (double)m.bsRadius
                                       : m.planetRadius * 2.0 / double(1u << m.lod);
        if (glm::length(c - center) < radius + r) {
            hit.push_back(m.key);
            cleanupMesh(m);
            it = m_gpuMeshes.erase(it);
        } else {
            ++it;
        }
    }
    return hit;
}

std::vector<PlanetChunkKey> TerrainRenderer::markStaleSphere(const glm::dvec3& center, double radius) {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    std::vector<PlanetChunkKey> hit;
    for (auto& [hash, m] : m_gpuMeshes) {
        glm::dvec3 c = m.chunkCenter + m_planetCenter + glm::dvec3(m.bsCenter);
        double r = (m.bsRadius > 0.0f) ? (double)m.bsRadius
                                       : m.planetRadius * 2.0 / double(1u << m.lod);
        if (glm::length(c - center) < radius + r) {
            m_stale.insert(hash);   // se reemplaza cuando regenere; mientras tanto SE SIGUE DIBUJANDO
            hit.push_back(m.key);
        }
    }
    return hit;
}

void TerrainRenderer::cleanupMesh(RenderMesh& mesh) {
    if (mesh.vao) glDeleteVertexArrays(1, &mesh.vao);
    if (mesh.vbo) glDeleteBuffers(1, &mesh.vbo);
    if (mesh.nbo) glDeleteBuffers(1, &mesh.nbo);
    if (mesh.uvo) glDeleteBuffers(1, &mesh.uvo);
    if (mesh.ebo) glDeleteBuffers(1, &mesh.ebo);
}

} // namespace Haruka