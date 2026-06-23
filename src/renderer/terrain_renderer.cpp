#include "terrain_renderer.h"
#include <cstdio>
#include <cstdlib>     // std::abs
#include <functional>  // std::function (recursión de covered())

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

    // Attr 4: morph-target normals (CDLOD) → la normal se mezcla con la posición en el
    // shader; sin esto el chunk aplanado por morph conserva normales bumpy → escalón.
    if (!data.morphNormals.empty()) {
        glGenBuffers(1, &mesh.nmbo);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.nmbo);
        glBufferData(GL_ARRAY_BUFFER, data.morphNormals.size() * sizeof(glm::vec3), data.morphNormals.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
        glEnableVertexAttribArray(4);
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

    // DEBE coincidir con el splitFactor REAL del LODSystem (setSplitFactor). Una hoja de
    // LOD L es visible en la banda de distancia [size*sf, 2*size*sf]; morphamos en su mitad
    // superior y llegamos a 1 justo en la frontera de fusión (2*size*sf) → sin parches lisos
    // ni popping. Antes estaba hardcodeado a 1.0 → desajustado con medium(0.85)/ultra(1.2).
    const double splitFactor = m_splitFactor;

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

    // --- SELECCIÓN DE DIBUJO DESACOPLADA DEL LOD --------------------------------------
    // El render NO recalcula cobertura por residencia (eso acoplaba el detalle a tener la
    // cadena ENTERA residente → mucha RAM + láminas por huecos). En su lugar usa el set de
    // HOJAS deseadas del LOD (partición balanceada) y, por cada hoja, elige el chunk residente
    // MÁS FINO disponible: la hoja si está, si no su ancestro residente más cercano (fallback
    // sin hueco). Así cerca llegas al fino en cuanto su hoja carga, sin niveles intermedios.
    std::unordered_set<uint64_t> drawnHashes;
    {
        auto dit = m_desiredLeaves.find(planet);
        if (dit != m_desiredLeaves.end()) {
            drawnHashes.reserve(dit->second.size());
            for (PlanetChunkKey k : dit->second) {
                while (true) {
                    uint64_t h = ChunkCache::keyToHash(k);
                    auto it = m_gpuMeshes.find(h);
                    if (it != m_gpuMeshes.end() && it->second.isReady) { drawnHashes.insert(h); break; }
                    if (k.lod == 0) break;                 // ni la raíz está → nada que dibujar aquí
                    k = { k.face, (uint8_t)(k.lod - 1), k.x >> 1, k.y >> 1, k.body }; // sube al padre
                }
            }
        }
    }

    int drawn = 0;
    for (auto& [hash, mesh] : m_gpuMeshes) {
        if (!mesh.isReady || mesh.planetName != planet) continue;

        // Solo se dibuja si es el chunk elegido para alguna hoja deseada (hoja o fallback).
        if (!drawnHashes.count(hash)) continue;

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
                    // MARGEN por tamaño del chunk: el test usa el CENTRO, pero un chunk grueso
                    // (lod bajo) abarca un arco enorme; su centro puede caer tras el horizonte
                    // mientras su borde cercano es visible. Sin margen, lo descartaba entero →
                    // "parte del planeta sin renderizar". Solo descartamos si TODO su arco está
                    // tras el horizonte (cos relajado por el radio angular) y su punto más
                    // cercano supera la distancia de horizonte.
                    double nodeSize  = mesh.planetRadius * 2.0 / double(1u << mesh.lod);
                    double angMargin = nodeSize / mesh.planetRadius; // radio angular generoso
                    if (cosChunk < cosHorizon - angMargin && chunkDist > horizonDist + nodeSize) continue;
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
        // Morph SOLO en el 40% superior de la banda (antes 50%): los chunks conservan el
        // detalle fino más lejos → menos "parche liso" a media distancia, y aun así el morph
        // llega a 1 en la frontera de fusión (t=1) → sin popping al fusionar.
        double morph    = (t - 0.6) / 0.4;
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

TerrainRenderer::LODReport TerrainRenderer::validateCoverage() const {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    LODReport r;

    auto H = [](const PlanetChunkKey& k) { return ChunkCache::keyToHash(k); };
    auto residentReady = [&](const PlanetChunkKey& k) -> bool {
        auto it = m_gpuMeshes.find(H(k));
        return it != m_gpuMeshes.end() && it->second.isReady;
    };

    // 1. Conjunto DIBUJADO = MISMA selección que renderPlanet: por cada hoja deseada, el chunk
    //    residente más fino (hoja o ancestro más cercano). SIN culling (valida la teselación).
    std::unordered_set<uint64_t> drawn;
    std::vector<PlanetChunkKey>  drawnKeys;
    std::unordered_set<uint16_t> bodies;
    for (const auto& [planet, leaves] : m_desiredLeaves) {
        for (PlanetChunkKey k : leaves) {
            while (true) {
                if (residentReady(k)) {
                    uint64_t h = H(k);
                    if (drawn.insert(h).second) { drawnKeys.push_back(k); bodies.insert(k.body); }
                    break;
                }
                if (k.lod == 0) break;
                k = { k.face, (uint8_t)(k.lod - 1), k.x >> 1, k.y >> 1, k.body };
            }
        }
    }
    r.drawn = (int)drawnKeys.size();
    for (const auto& k : drawnKeys) if (k.body < 16) r.drawnByBody[k.body]++;

    // 2. OVERLAP: ¿algún ancestro del chunk dibujado también está dibujado? → doble malla.
    for (const auto& k : drawnKeys) {
        PlanetChunkKey a = k;
        while (a.lod > 0) {
            a = {a.face,(uint8_t)(a.lod-1),a.x>>1,a.y>>1,a.body};
            if (drawn.count(H(a))) {
                r.overlaps++;
                if (k.body < 16) r.overlapsByBody[k.body]++;
                if (!r.hasOverlap){r.hasOverlap=true;r.sampleOverlap=k;}
                break;
            }
        }
    }

    // 3. HOLES: cada cara (lod 0) debe quedar TOTALMENTE cubierta. internal = nodos con
    //    descendientes dibujados; un nodo sin dibujar y sin descendiente dibujado = hueco.
    std::unordered_set<uint64_t> internal;
    for (const auto& k : drawnKeys) {
        PlanetChunkKey a = k;
        while (a.lod > 0) { a = {a.face,(uint8_t)(a.lod-1),a.x>>1,a.y>>1,a.body}; internal.insert(H(a)); }
    }
    std::function<bool(const PlanetChunkKey&)> covered = [&](const PlanetChunkKey& n) -> bool {
        if (drawn.count(H(n))) return true;
        if (internal.count(H(n))) {
            const uint8_t cl = (uint8_t)(n.lod+1); const uint32_t bx = n.x*2, by = n.y*2; const uint16_t bd = n.body;
            bool ok = true;
            const PlanetChunkKey kids[4] = {{n.face,cl,bx,by,bd},{n.face,cl,bx+1,by,bd},
                                            {n.face,cl,bx,by+1,bd},{n.face,cl,bx+1,by+1,bd}};
            for (const auto& c : kids) if (!covered(c)) ok = false; // sin short-circuit → cuenta todos
            return ok;
        }
        r.holes++; if (!r.hasHole) { r.hasHole = true; r.sampleHole = n; } // ni dibujado ni subdividido
        return false;
    };
    for (uint16_t bd : bodies)
        for (int f = 0; f < 6; ++f) covered({(PlanetFace)f, 0, 0, 0, bd});

    // 4. BALANCE 2:1: cada vecino (misma cara) debe diferir ≤1 nivel. Cruces de cara: omitido.
    auto coveringDrawn = [&](PlanetFace f, uint16_t bd, int lod, uint32_t x, uint32_t y, PlanetChunkKey& out) -> bool {
        for (int l = lod; l >= 0; --l) {
            PlanetChunkKey k{f,(uint8_t)l, x>>(lod-l), y>>(lod-l), bd};
            if (drawn.count(H(k))) { out = k; return true; }
        }
        return false;
    };
    const int dx[4] = {-1,1,0,0}, dy[4] = {0,0,-1,1};
    for (const auto& k : drawnKeys) {
        const long n = 1L << k.lod;
        for (int d = 0; d < 4; ++d) {
            long nx = (long)k.x + dx[d], ny = (long)k.y + dy[d];
            if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue; // cross-face
            PlanetChunkKey cov;
            if (!coveringDrawn(k.face, k.body, k.lod, (uint32_t)nx, (uint32_t)ny, cov)) continue;
            if (std::abs((int)k.lod - (int)cov.lod) > 1) {
                r.balance++; if (!r.hasBalance) { r.hasBalance = true; r.sampleBalance = k; }
            }
        }
    }
    return r;
}

void TerrainRenderer::cleanupMesh(RenderMesh& mesh) {
    if (mesh.vao) glDeleteVertexArrays(1, &mesh.vao);
    if (mesh.vbo) glDeleteBuffers(1, &mesh.vbo);
    if (mesh.mbo) glDeleteBuffers(1, &mesh.mbo);   // antes NO se borraba → fuga de buffer GPU
    if (mesh.nbo) glDeleteBuffers(1, &mesh.nbo);
    if (mesh.nmbo) glDeleteBuffers(1, &mesh.nmbo);
    if (mesh.uvo) glDeleteBuffers(1, &mesh.uvo);
    if (mesh.ebo) glDeleteBuffers(1, &mesh.ebo);
}

} // namespace Haruka