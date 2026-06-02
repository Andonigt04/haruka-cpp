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

        // Distance to the chunk CENTRE (on the reference sphere → height-independent).
        glm::dvec3 toCenter = mesh.chunkCenter - glm::dvec3(cameraPos);
        glm::vec3  offset    = glm::vec3(toCenter);

        // Horizon cull: hide chunks beyond the planet's curvature. occR uses the
        // chunk's planetRadius minus a generous margin for the tallest peaks
        // (~11 km) + slack. Only engages when the camera is clearly ABOVE that
        // shell — at/near ground level the horizon is metres away and the test
        // would wrongly cull nearby visible chunks.
        if (m_hasPlanetCenter) {
            double occR = mesh.planetRadius - 15000.0;
            if (occR > 1.0 && camDist > occR + 2000.0) {
                glm::dvec3 chunkFromCenter = mesh.chunkCenter - m_planetCenter;
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

void TerrainRenderer::cleanupMesh(RenderMesh& mesh) {
    if (mesh.vao) glDeleteVertexArrays(1, &mesh.vao);
    if (mesh.vbo) glDeleteBuffers(1, &mesh.vbo);
    if (mesh.nbo) glDeleteBuffers(1, &mesh.nbo);
    if (mesh.uvo) glDeleteBuffers(1, &mesh.uvo);
    if (mesh.ebo) glDeleteBuffers(1, &mesh.ebo);
}

}