#include "terrain_renderer.h"
#include "core/terrain/shared_index_table.h" // índices compartidos por res
#include "tools/profiler.h"                   // HARUKA_PROFILE (sub-scopes de terrain.draw)
#include <cstdio>
#include <cstdlib>     // std::abs, getenv
#include <functional>  // std::function (recursión de covered())
#include <chrono>      // micro-bench del splice
#include <algorithm>   // std::max

namespace Haruka {

namespace {
    // Inverso EXACTO de ChunkCache::keyToHash (empaquetado, no hash con colisiones):
    // face(3) | lod(5) | x(23) | y(23) | body(10).
    inline PlanetChunkKey hashToKey(uint64_t h) {
        PlanetChunkKey k;
        k.face = (PlanetFace)(h & 0x7);
        k.lod  = (uint8_t)((h >> 3) & 0x1F);
        k.x    = (uint32_t)((h >> 8) & 0x7FFFFF);
        k.y    = (uint32_t)((h >> 31) & 0x7FFFFF);
        k.body = (uint16_t)((h >> 54) & 0x3FF);
        return k;
    }
    // Ancestro de k reducido al nivel `lod` (lod <= k.lod). Misma cara/cuerpo.
    inline PlanetChunkKey ancestorAtLod(const PlanetChunkKey& k, uint8_t lod) {
        uint32_t shift = (uint32_t)(k.lod - lod);
        return { k.face, lod, k.x >> shift, k.y >> shift, k.body };
    }
    // ¿a es ancestro-o-igual de b? (a cubre el área de b)
    inline bool isAncestorOrEqual(const PlanetChunkKey& a, const PlanetChunkKey& b) {
        if (a.body != b.body || a.face != b.face || a.lod > b.lod) return false;
        uint32_t shift = (uint32_t)(b.lod - a.lod);
        return (b.x >> shift) == a.x && (b.y >> shift) == a.y;
    }
}

TerrainRenderer::~TerrainRenderer() {
    for (auto& pair : m_gpuMeshes) {
        cleanupMesh(pair.second);
    }
    for (auto& [vc, p] : m_pools) {   // pools de batching (el EBO es compartido → NO se borra aquí)
        if (p.vao) glDeleteVertexArrays(1, &p.vao);
        GLuint bufs[5] = { p.posVBO, p.morphVBO, p.normVBO, p.morphNormVBO, p.uvVBO };
        glDeleteBuffers(5, bufs);
        if (p.cmdBuf)   glDeleteBuffers(1, &p.cmdBuf);
        if (p.drawSSBO) glDeleteBuffers(1, &p.drawSSBO);
    }
    for (auto& [ic, ebo] : m_sharedEBO) if (ebo) glDeleteBuffers(1, &ebo); // EBOs compartidos
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

    // TOPE DURO de chunks residentes (acota RAM/VRAM). Un pico (fly-in / descenso orbital) hacía
    // que el LOD pidiera DECENAS DE MILES de chunks → los pools crecen por duplicación y NO encogen
    // → decenas de GB que desbordan la GPU a la RAM (el "llega a 32 GB"). Si ya estamos al tope y el
    // chunk es NUEVO, no lo subimos: se queda en cache y su ANCESTRO grueso residente lo dibuja (sin
    // hueco, solo menos detalle lejano). La eviction libera hueco y entra cuando de verdad toca.
    if ((int)m_gpuMeshes.size() >= kMaxResidentChunks) return;

    RenderMesh mesh;
    // indexCount conservado tras liberar `indices`; fallback a indices.size() si un chunk no pasó
    // por el generador (p.ej. carga directa) y aún lleva sus índices.
    mesh.indexCount  = data.indexCount ? data.indexCount : (uint32_t)data.indices.size();
    mesh.vertexCount = (uint32_t)data.vertices.size();
    mesh.planetName  = planetName;
    mesh.key         = key;
    mesh.chunkCenter = data.chunkCenter;
    mesh.terrainMode = static_cast<int>(data.terrainMode);
    mesh.lod         = data.key.lod;
    mesh.planetRadius = data.planetRadius;

    // Bounding sphere de los vértices de superficie (sin skirts, que cuelgan y la inflarían).
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

    // --- CAMINO BATCHING: subir al POOL compartido (sin VAO propio) --------------------
    // Solo si el chunk trae TODOS los atributos a resolución completa y su topología está en
    // la tabla de índices compartida (garantiza que el EBO del pool tiene datos). Si falta
    // algo (chunk raro / carga directa), cae al camino legacy de abajo — modos mezclados OK.
    const bool canPool = m_batching
        && !data.vertices.empty()
        && data.morphTargets.size()       == data.vertices.size()
        && data.normalsPacked.size()      == data.vertices.size()
        && data.morphNormalsPacked.size() == data.vertices.size()
        && data.uvsPacked.size()          == data.vertices.size()
        && SharedIndexTable::get().find(mesh.indexCount) != nullptr;
    if (canPool) {
        ChunkPool& p = getOrCreatePool(mesh.vertexCount, mesh.indexCount);
        if (p.freeSlots.empty()) growPool(p);
        uint32_t slot = p.freeSlots.back(); p.freeSlots.pop_back();
        const size_t vc   = mesh.vertexCount;
        const size_t base = (size_t)slot * vc;
        auto sub = [&](GLuint buf, size_t elem, const void* src) {
            glBindBuffer(GL_ARRAY_BUFFER, buf);
            glBufferSubData(GL_ARRAY_BUFFER, base * elem, vc * elem, src);
        };
        sub(p.posVBO,       sizeof(glm::vec3), data.vertices.data());
        sub(p.morphVBO,     sizeof(glm::vec3), data.morphTargets.data());
        sub(p.normVBO,      sizeof(uint32_t),  data.normalsPacked.data());
        sub(p.morphNormVBO, sizeof(uint32_t),  data.morphNormalsPacked.data());
        sub(p.uvVBO,        sizeof(uint32_t),  data.uvsPacked.data());
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        mesh.poolSlot = (int32_t)slot;
        mesh.poolKey  = mesh.vertexCount;
        mesh.vao      = 0;               // sin buffers propios
        mesh.isReady  = true;
        m_gpuMeshes[hash] = mesh;
        noteResidencyChange(hash);       // invalida el draw-set SOLO si el chunk está en la clausura
        purgeStaleCoveredBy(key);        // retira mallas stale ya cubiertas (sin agujero)
        return;
    }

    glGenVertexArrays(1, &mesh.vao);
    glBindVertexArray(mesh.vao);

    // Attr 0: positions (float, relativo a chunkCenter)
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

    // Attr 1: normals — EMPAQUETADAS (#4): INT_2_10_10_10 (4B). El shader lee `vec3 aNormal`
    // y GL des-empaqueta normalizado (xyz) → sin cambios en el shader.
    if (!data.normalsPacked.empty()) {
        glGenBuffers(1, &mesh.nbo);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.nbo);
        glBufferData(GL_ARRAY_BUFFER, data.normalsPacked.size() * sizeof(uint32_t), data.normalsPacked.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(1, 4, GL_INT_2_10_10_10_REV, GL_TRUE, sizeof(uint32_t), nullptr);
        glEnableVertexAttribArray(1);
    }

    // Attr 4: morph-target normals (CDLOD), también empaquetadas.
    if (!data.morphNormalsPacked.empty()) {
        glGenBuffers(1, &mesh.nmbo);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.nmbo);
        glBufferData(GL_ARRAY_BUFFER, data.morphNormalsPacked.size() * sizeof(uint32_t), data.morphNormalsPacked.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(4, 4, GL_INT_2_10_10_10_REV, GL_TRUE, sizeof(uint32_t), nullptr);
        glEnableVertexAttribArray(4);
    }

    // Attr 2: UVs — half-float (#4): 2× half (4B vs 8B). El shader lee `vec2 aUV` (auto-convert).
    if (!data.uvsPacked.empty()) {
        glGenBuffers(1, &mesh.uvo);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.uvo);
        glBufferData(GL_ARRAY_BUFFER, data.uvsPacked.size() * sizeof(uint32_t), data.uvsPacked.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(2, 2, GL_HALF_FLOAT, GL_FALSE, sizeof(uint32_t), nullptr);
        glEnableVertexAttribArray(2);
    }

    // EBO COMPARTIDO: los índices son topología FIJA por nº de índices (grid + skirts del
    // mismo res) → idénticos en TODOS los chunks de ese res. Un único EBO por indexCount,
    // reusado por todos → ahorra ~6·res²·4 B × miles de chunks en VRAM. El VAO (ligado abajo)
    // recuerda este EBO; cleanupMesh NO lo borra (es compartido; se liberan en el destructor).
    {
        uint32_t ic = mesh.indexCount; // (indexCount conservado o fallback a indices.size())
        auto it = m_sharedEBO.find(ic);
        if (it == m_sharedEBO.end()) {
            // Datos: tabla compartida (camino normal) o los índices propios del chunk (fallback).
            const std::vector<unsigned int>* idx = SharedIndexTable::get().find(ic);
            const unsigned int* src = idx ? idx->data()
                                          : (data.indices.empty() ? nullptr : data.indices.data());
            GLuint ebo = 0;
            glGenBuffers(1, &ebo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, ic * sizeof(unsigned int), src, GL_STATIC_DRAW);
            it = m_sharedEBO.emplace(ic, ebo).first;
        } else {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, it->second); // ligarlo al VAO actual
        }
        mesh.ebo = it->second; // referencia (NO se posee → no se borra en cleanupMesh)
    }

    mesh.isReady = true;
    m_gpuMeshes[hash] = mesh;
    noteResidencyChange(hash);   // invalida el draw-set SOLO si el chunk está en la clausura

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
        noteResidencyChange(hash);           // invalida el draw-set SOLO si el chunk está en la clausura
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
                noteResidencyChange(ph);
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
        noteResidencyChange(h);
        if (m_onRemoved) m_onRemoved(dk);   // espejo: quita su agua
    }
}

void TerrainRenderer::render(const Haruka::WorldPos& cameraPos) {
    std::lock_guard<std::mutex> lock(m_renderMutex);

    GLint prog = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    GLint loc = glGetUniformLocation(prog, "u_chunkOffset");

    GLuint boundVao = 0;
    for (auto& [hash, mesh] : m_gpuMeshes) {
        if (!mesh.isReady) continue;
        if (loc >= 0) {
            glm::vec3 offset = glm::vec3(mesh.chunkCenter - glm::dvec3(cameraPos));
            glUniform3fv(loc, 1, &offset[0]);
        }
        GLuint wantVao = mesh.vao;
        GLint  baseVertex = 0;
        if (mesh.poolSlot >= 0) {
            auto pit = m_pools.find(mesh.poolKey);
            if (pit == m_pools.end()) continue;
            wantVao    = pit->second.vao;
            baseVertex = (GLint)((size_t)mesh.poolSlot * mesh.vertexCount);
        }
        if (wantVao != boundVao) { glBindVertexArray(wantVao); boundVao = wantVao; }
        if (mesh.poolSlot >= 0)
            glDrawElementsBaseVertex(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, nullptr, baseVertex);
        else
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
    // CACHE del draw set: buildDrawSet es CARO (~varios ms: recursión + sets hash sobre miles de
    // nodos). Su resultado SOLO depende de (hojas deseadas, conjunto residente). Reusamos el cache
    // si ninguno cambió desde la última construcción → quieto/asentado NO reconstruye. El culling
    // (depende de la cámara) y el dibujo SÍ corren cada frame sobre el set cacheado.
    const bool dirty = m_drawSetDirty[planet] != 0;
    auto  verIt = m_drawSetCacheVer.find(planet);
    const bool verOk = (verIt != m_drawSetCacheVer.end() && verIt->second == m_residentVersion);
    auto  cacheIt = m_drawSetCache.find(planet);
    const bool haveCache = (cacheIt != m_drawSetCache.end());
    // Frame en que SOLO cambió la residencia (hojas iguales, hay cache): intentamos el DELTA-SPLICE
    // (re-emitir solo los subárboles tocados) en vez de reconstruir todo. Si el delta no aplica
    // (grande / sin fallback residente) caemos al rebuild completo.
    bool applied = false;
    if (!dirty && !verOk && haveCache) {
        HARUKA_PROFILE("terrain.drawset.delta"); // coste del splice (debería ser « rebuild completo)
        if (applyDrawSetDelta(planet)) {
            m_drawSetCacheVer[planet] = m_residentVersion;
            applied = true;
            ++m_dbgDeltaApplied;
        }
    }
    if (!applied && (dirty || !verOk || !haveCache)) {
        HARUKA_PROFILE("terrain.drawset"); // solo acumula en frames de REBUILD → delata si el coste es el rebuild
        if (!dirty && haveCache) ++m_dbgDeltaFull; // era elegible para delta pero cayó al rebuild
        std::unordered_set<uint64_t> fresh;
        buildDrawSet(planet, fresh, nullptr);    // partición top-down sin solapes (0 overlaps, 0 holes)
        cacheIt = m_drawSetCache.insert_or_assign(planet, std::move(fresh)).first;
        m_drawSetCacheVer[planet] = m_residentVersion;
        m_drawSetDirty[planet]    = 0;
        m_pendingDelta[planet].clear(); // el fresco ya refleja la residencia actual → deltas obsoletos
    } else {
        cacheIt = m_drawSetCache.find(planet); // applyDrawSetDelta mutó el cache in-place
    }
    const std::unordered_set<uint64_t>& drawnHashes = cacheIt->second;

    const GLint locBatched = 40; // u_batched en planet.vert (1 = leer datos por-draw del SSBO)
    std::unordered_set<uint64_t> visibleHashes; // chunks VISIBLES (post-cull) → sync del agua
    visibleHashes.reserve(drawnHashes.size());
    int drawn = 0;
    GLuint boundVao = 0; // batching: solo rebind del VAO cuando cambia (con el pool = 1 sola vez)
    glUniform1i(locBatched, 0);   // por defecto: draws normales (uniforms por chunk)
    // MultiDrawIndirect: acumulamos por pool los chunks VISIBLES (tras cull) y emitimos UNA
    // glMultiDrawElementsIndirect por pool → colapsa ~1500 draws en 1 (o pocas) llamadas. Solo
    // con m_batching ON; si OFF, los chunks pooled se dibujan inline con glDrawElementsBaseVertex
    // (camino validado) → fallback seguro con `terrainpool off`.
    for (auto& [k, v] : m_scratchCmds)  v.clear();
    for (auto& [k, v] : m_scratchItems) v.clear();
    // Iteramos el DRAW SET (~1500 chunks) en vez de TODAS las mallas residentes (~6400): antes
    // el bucle recorría todo m_gpuMeshes y descartaba las no-dibujadas con un lookup → ~5000
    // iteraciones+lookups desperdiciados por frame. buildDrawSet ya nos da exactamente los hashes
    // a dibujar → los buscamos directos.
    for (uint64_t hash : drawnHashes) {
        auto mit = m_gpuMeshes.find(hash);
        if (mit == m_gpuMeshes.end()) continue;
        RenderMesh& mesh = mit->second;
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
            // Margen 1.25× (antes 1.1): un chunk grueso abarca un arco y su esfera envolvente puede
            // quedarse corta en el borde de pantalla → se descartaba parcialmente visible = "terreno
            // que falta en ciertos ángulos". 1.25 dibuja un pelín de más en el borde (barato) sin culls falsos.
            float radius = mesh.bsRadius > 0.0f
                         ? mesh.bsRadius * 1.25f
                         : (float)(mesh.planetRadius * 2.0 / double(1u << mesh.lod));
            bool inside = true;
            for (int p = 0; p < 6; ++p) {
                float d = planes[p].x*c.x + planes[p].y*c.y + planes[p].z*c.z + planes[p].w;
                if (d < -radius) { inside = false; break; }
            }
            if (!inside) continue;
        }

        // VISIBLE (pasó horizon+frustum) → al set POST-CULL para el agua: el agua dibuja EXACTAMENTE
        // los chunks visibles del terreno (no la partición entera del hemisferio) → sin agua fuera de
        // cámara ni teselas lejanas sueltas ("por trozos indiscriminadamente"). Sincroniza cull agua↔terreno.
        visibleHashes.insert(hash);

        // Morph CDLOD (per-chunk): en el 40% superior de la banda de visibilidad del chunk.
        double dist     = glm::length(toCenter);
        double nodeSize = mesh.planetRadius * 2.0 / double(1u << mesh.lod);
        double low      = nodeSize * splitFactor;
        double high     = nodeSize * splitFactor * 2.0;
        double t        = (high > low) ? (dist - low) / (high - low) : 0.0;
        double morph    = (t - 0.6) / 0.4;
        morph = morph < 0.0 ? 0.0 : (morph > 1.0 ? 1.0 : morph);

        const bool pooled = (mesh.poolSlot >= 0);
        if (pooled && m_batching) {
            // Acumular para MultiDrawIndirect (una llamada por pool tras el bucle).
            GpuDrawItem it;
            it.offMorph[0] = offset.x; it.offMorph[1] = offset.y; it.offMorph[2] = offset.z;
            it.offMorph[3] = (float)morph;
            it.mode[0] = mesh.terrainMode; it.mode[1] = it.mode[2] = it.mode[3] = 0;
            GpuDrawCmd cmd;
            cmd.count         = mesh.indexCount;
            cmd.instanceCount = 1;
            cmd.firstIndex    = 0;
            cmd.baseVertex    = (uint32_t)((size_t)mesh.poolSlot * mesh.vertexCount);
            cmd.baseInstance  = 0;
            m_scratchCmds[mesh.poolKey].push_back(cmd);
            m_scratchItems[mesh.poolKey].push_back(it);
            continue; // se dibuja en el batch de su pool
        }

        // Camino inline (legacy sin pool, o pooled con batching OFF): draw normal por chunk.
        GLuint wantVao = mesh.vao;
        GLint  baseVertex = 0;
        if (pooled) {
            auto pit = m_pools.find(mesh.poolKey);
            if (pit == m_pools.end()) continue;
            wantVao    = pit->second.vao;
            baseVertex = (GLint)((size_t)mesh.poolSlot * mesh.vertexCount);
        }
        glUniform3fv(locOffset, 1, &offset[0]);
        glUniform1i(locMode, mesh.terrainMode);
        glUniform1f(locMorph, (float)morph);
        if (wantVao != boundVao) { glBindVertexArray(wantVao); boundVao = wantVao; }
        if (pooled)
            glDrawElementsBaseVertex(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, nullptr, baseVertex);
        else
            glDrawElements(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, nullptr);
        ++drawn;
    }

    // --- MultiDrawIndirect: una llamada por pool con todos sus chunks visibles ---------------
    if (m_batching) {
        HARUKA_PROFILE("terrain.emit"); // orphan+upload de cmd/SSBO + glMultiDrawElementsIndirect por pool
        glUniform1i(locBatched, 1);
        for (auto& [poolKey, cmds] : m_scratchCmds) {
            if (cmds.empty()) continue;
            auto pit = m_pools.find(poolKey);
            if (pit == m_pools.end()) continue;
            ChunkPool& pool = pit->second;
            auto& items = m_scratchItems[poolKey];
            const size_t n = cmds.size();
            if (pool.cmdBuf == 0)   glGenBuffers(1, &pool.cmdBuf);
            if (pool.drawSSBO == 0) glGenBuffers(1, &pool.drawSSBO);
            // ORPHAN + upload (glBufferData) cada frame: descarta el buffer viejo → el driver da uno
            // nuevo sin esperar a que la MultiDraw del frame anterior lo suelte (evita el stall
            // implícito de glBufferSubData sobre un buffer aún en uso). ~48 KB → barato.
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, pool.cmdBuf);
            glBufferData(GL_DRAW_INDIRECT_BUFFER, n * sizeof(GpuDrawCmd), cmds.data(), GL_STREAM_DRAW);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, pool.drawSSBO);
            glBufferData(GL_SHADER_STORAGE_BUFFER, n * sizeof(GpuDrawItem), items.data(), GL_STREAM_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, pool.drawSSBO);
            glBindVertexArray(pool.vao);
            glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, (GLsizei)n, 0);
            drawn += (int)n;
        }
        glUniform1i(locBatched, 0);
    }
    glBindVertexArray(0);
    m_lastDrawn = drawn;
    m_drawnByPlanet[planet] = std::move(visibleHashes); // set POST-CULL → el agua dibuja solo lo visible

    // EVICTION (acota el conjunto residente): antes los chunks que salían de vista al EXPLORAR se
    // marcaban stale pero su "reemplazo" (subdividir/fusionar) NUNCA ocurría (ya no estás ahí) →
    // quedaban residentes PARA SIEMPRE → RAM/VRAM sin tope (los pools reservan VRAM proporcional →
    // desbordaba a RAM = thrashing). Ahora: envejecemos las mallas de este planeta que NO están en
    // el set deseado y purgamos las que llevan mucho sin verse. Cada ~20 frames (barato) y NUNCA
    // toca lo que el LOD quiere ahora (framesUndrawn=0 para las del set). Cascada al agua vía
    // m_onRemoved. Al volver, re-stremean (pop-in breve) — comportamiento normal de streaming.
    if (((++m_ageTick) % 20) == 0) {
        HARUKA_PROFILE("terrain.evict"); // cada 20 frames: itera TODAS las mallas residentes → posible pico
        const int MAX_UNDRAWN = 45; // 45·20 = 900 frames (~15-27 s) fuera del área deseada → fuera. SUBIDO
                                    // de 12 (~4s): al GIRAR EN SITIO, los LODs intermedios/finos de las
                                    // direcciones que sales de vista NO se borran → al volver a mirarlas
                                    // siguen residentes (no hay que re-trepar la jerarquía 10-15s = "el
                                    // terreno desaparece al girar"). Memoria de sobra; el cap acota igual.
        // "needed" = hojas deseadas + TODOS sus ANCESTROS (el subárbol del área actual). CLAVE:
        // conservar los ancestros gruesos → son el fallback SIN HUECO cuando una hoja fina aún no
        // está residente (streaming). Antes purgaba por drawnHashes (solo hojas) → quitaba esos
        // ancestros → huecos grandes de terreno al refinar. Solo se purga lo de zonas que dejaste.
        std::unordered_set<uint64_t> needed;
        auto dit = m_desiredLeaves.find(planet);
        if (dit != m_desiredLeaves.end()) {
            needed.reserve(dit->second.size() * 2);
            for (const auto& lf : dit->second) {
                needed.insert(ChunkCache::keyToHash(lf));
                PlanetChunkKey a = lf;
                while (a.lod > 0) { a = { a.face, (uint8_t)(a.lod - 1), a.x >> 1, a.y >> 1, a.body }; needed.insert(ChunkCache::keyToHash(a)); }
            }
        }
        // KEEP-RADIUS: conservar TODO lo cercano (dentro del horizonte) aunque esté DETRÁS de la
        // cámara. El set `needed` solo cubre lo que MIRAS ahora (frustum) → al girar en sitio, los
        // chunks de detrás (cerca, potencialmente visibles al rotar) se evictaban → al volver a
        // mirarlos, re-generar. Manteniéndolos por DISTANCIA, girar en sitio los encuentra listos.
        // Solo evicta lo LEJANO que dejaste atrás al MOVERTE (el radio sigue a la cámara). Tope para
        // no retener un casquete enorme en altura (allí ya ves el planeta entero, no aplica el bug).
        std::vector<uint64_t> purge;
        if (!needed.empty()) {  // sin set deseado (carga/transición) NO evictamos → no borrar todo
        const glm::dvec3 camW = glm::dvec3(cameraPos);
        for (auto& [h, m] : m_gpuMeshes) {
            if (m.planetName != planet) continue;
            // keepR = horizonte (lo que podrías ver al girar), del radio del propio chunk (igual para
            // todo el planeta). Tope 15 km para no retener un casquete enorme en altura.
            const double horizon = std::sqrt(std::max(0.0, camDist * camDist - m.planetRadius * m.planetRadius));
            const double keepR   = std::min(horizon * 1.05 + 300.0, 15000.0);
            glm::dvec3 d = (m.chunkCenter + m_planetCenter) - camW;
            const bool nearCam = (glm::dot(d, d) < keepR * keepR);
            if (needed.count(h) || nearCam) m.framesUndrawn = 0;
            else if (++m.framesUndrawn > MAX_UNDRAWN) purge.push_back(h);
        }
        }
        for (uint64_t h : purge) {
            auto it = m_gpuMeshes.find(h);
            if (it == m_gpuMeshes.end()) continue;
            PlanetChunkKey pk = it->second.key;
            cleanupMesh(it->second);
            m_gpuMeshes.erase(it);
            m_stale.erase(h);
            noteResidencyChange(h);
            if (m_onRemoved) m_onRemoved(pk); // espejo: quita su agua
        }
    }
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
            const uint64_t h = it->first; // capturar antes de borrar el iterador
            hit.push_back(m.key);
            cleanupMesh(m);
            it = m_gpuMeshes.erase(it);
            noteResidencyChange(h);
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

bool TerrainRenderer::debugCachedDrawSetIsFresh(const std::string& planet) const {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    auto it = m_drawSetCache.find(planet);
    if (it == m_drawSetCache.end()) return true;   // nada cacheado aún → nada que validar
    std::unordered_set<uint64_t> fresh;
    buildDrawSet(planet, fresh, nullptr);
    return it->second == fresh;
}

void TerrainRenderer::benchDrawSetSplice(const std::string& planet, int iters,
                                         double* nsFull, double* nsSplice, size_t* drawn) {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    if (nsFull) *nsFull = 0; if (nsSplice) *nsSplice = 0; if (drawn) *drawn = 0;
    // Asegurar un cache válido de base.
    auto cIt = m_drawSetCache.find(planet);
    if (cIt == m_drawSetCache.end()) {
        std::unordered_set<uint64_t> fresh;
        buildDrawSet(planet, fresh, nullptr);
        cIt = m_drawSetCache.insert_or_assign(planet, std::move(fresh)).first;
        m_drawSetCacheVer[planet] = m_residentVersion; m_drawSetDirty[planet] = 0;
    }
    if (drawn) *drawn = cIt->second.size();
    using clk = std::chrono::high_resolution_clock;
    // FULL: buildDrawSet completo en un temporal.
    {
        std::unordered_set<uint64_t> tmp; tmp.reserve(cIt->second.size());
        auto t0 = clk::now();
        for (int i = 0; i < iters; ++i) { tmp.clear(); buildDrawSet(planet, tmp, nullptr); }
        auto t1 = clk::now();
        if (nsFull) *nsFull = std::chrono::duration<double, std::nano>(t1 - t0).count() / std::max(1, iters);
    }
    // SPLICE: un cambio sintético de 1 nodo (una hoja residente) → applyDrawSetDelta, restaurando el
    // cache/pending entre iteraciones para medir siempre el mismo trabajo.
    uint64_t victim = 0;
    auto lsIt = m_leafSet.find(planet);
    if (lsIt != m_leafSet.end()) {
        for (uint64_t h : lsIt->second) {
            auto git = m_gpuMeshes.find(h);
            if (git != m_gpuMeshes.end() && git->second.isReady) { victim = h; break; }
        }
    }
    if (victim) {
        const std::unordered_set<uint64_t> savedCache   = cIt->second;
        auto savedPending = m_pendingDelta.count(planet) ? m_pendingDelta[planet] : std::unordered_set<uint64_t>{};
        auto t0 = clk::now();
        for (int i = 0; i < iters; ++i) {
            m_drawSetCache[planet] = savedCache;
            m_pendingDelta[planet] = { victim };
            applyDrawSetDelta(planet);
        }
        auto t1 = clk::now();
        if (nsSplice) *nsSplice = std::chrono::duration<double, std::nano>(t1 - t0).count() / std::max(1, iters);
        m_drawSetCache[planet]   = savedCache;      // restaurar estado
        m_pendingDelta[planet]   = savedPending;
    }
}

void TerrainRenderer::rebuildClosureForPlanet(const std::string& planet) {
    // CALLER HOLDS m_renderMutex. Clausura = hojas ∪ ancestros ∪ raíces de cara (por cuerpo). Es el
    // conjunto EXACTO de nodos cuya residencia puede alterar buildDrawSet (ver comentario en el .h).
    // Coste: como el pase de construcción de buildDrawSet (early-out de trie) → barato; solo corre
    // al cambiar las hojas (throttleado).
    auto& clo = m_closureByPlanet[planet];
    clo.clear();
    // Sets PRECOMPUTADOS para emitSubtree (estáticos hasta el próximo cambio de hojas): hojas y nodos
    // internos (ancestros de hojas). Antes buildDrawSet reconstruía el trie de internos CADA frame;
    // ahora se hace aquí (solo al cambiar hojas, throttleado) y el delta/emit lo reusa.
    auto& leafSet = m_leafSet[planet]; leafSet.clear();
    auto& intSet  = m_intSet[planet];  intSet.clear();
    auto dit = m_desiredLeaves.find(planet);
    if (dit != m_desiredLeaves.end()) {
        const auto& leaves = dit->second;
        clo.reserve(leaves.size() * 4);
        leafSet.reserve(leaves.size());
        intSet.reserve(leaves.size() * 2);
        std::unordered_set<uint16_t> bodies;
        for (const PlanetChunkKey& lf : leaves) {
            bodies.insert(lf.body);
            uint64_t lh = ChunkCache::keyToHash(lf);
            clo.insert(lh);                                      // la hoja
            leafSet.insert(lh);
            PlanetChunkKey a = lf;                               // sus ancestros (early-out de trie)
            while (a.lod > 0) {
                a = { a.face, (uint8_t)(a.lod - 1), a.x >> 1, a.y >> 1, a.body };
                uint64_t ah = ChunkCache::keyToHash(a);
                intSet.insert(ah);
                if (!clo.insert(ah).second) break;               // ya estaba → arriba también
            }
        }
        for (uint16_t bd : bodies) {                            // raíces de cara (por si se generaran a lod0)
            m_bodyToPlanet[bd] = planet;                        // routing de noteResidencyChange
            for (int f = 0; f < 6; ++f)
                clo.insert(ChunkCache::keyToHash({ (PlanetFace)f, 0, 0, 0, bd }));
        }
    }
    rebuildClosureAll();
}

void TerrainRenderer::rebuildClosureAll() {
    // CALLER HOLDS m_renderMutex. Unión de todas las clausuras por planeta (pocos planetas).
    m_closureAll.clear();
    size_t total = 0;
    for (const auto& [p, s] : m_closureByPlanet) total += s.size();
    m_closureAll.reserve(total);
    for (const auto& [p, s] : m_closureByPlanet) m_closureAll.insert(s.begin(), s.end());
}

void TerrainRenderer::buildDrawSet(const std::string& planet,
                                   std::unordered_set<uint64_t>& outHashes,
                                   std::vector<PlanetChunkKey>* outKeys,
                                   std::unordered_set<uint16_t>* outBodies) const {
    auto dit = m_desiredLeaves.find(planet);
    if (dit == m_desiredLeaves.end() || dit->second.empty()) return;
    const auto& leaves = dit->second;

    auto H      = [](const PlanetChunkKey& k){ return ChunkCache::keyToHash(k); };
    auto parent = [](const PlanetChunkKey& k){ return PlanetChunkKey{k.face,(uint8_t)(k.lod-1),k.x>>1,k.y>>1,k.body}; };
    auto kids   = [](const PlanetChunkKey& n, PlanetChunkKey c[4]){
        const uint8_t cl=(uint8_t)(n.lod+1); const uint32_t bx=n.x*2, by=n.y*2; const uint16_t bd=n.body;
        c[0]={n.face,cl,bx,by,bd};   c[1]={n.face,cl,bx+1,by,bd};
        c[2]={n.face,cl,bx,by+1,bd}; c[3]={n.face,cl,bx+1,by+1,bd};
    };
    auto residentReady = [&](const PlanetChunkKey& k)->bool{
        auto it = m_gpuMeshes.find(H(k));
        return it != m_gpuMeshes.end() && it->second.isReady;
    };

    // CAMINO RÁPIDO: si TODAS las hojas deseadas están residentes (lo normal moviéndote por zona
    // ya cargada), el draw set ES el conjunto de hojas — sin fallback a ancestros, sin la recursión
    // canCover/emit ni los sets `internal`/`memo` (decenas de miles de ops hash). ~0.1ms vs ~5ms.
    // Correcto: si todas residentes, emit bajaría hasta cada hoja y la dibujaría → mismo resultado.
    {
        bool allResident = true;
        for (const PlanetChunkKey& lf : leaves) if (!residentReady(lf)) { allResident = false; break; }
        if (allResident) {
            outHashes.reserve(leaves.size());
            for (const PlanetChunkKey& lf : leaves) {
                if (outHashes.insert(H(lf)).second && outKeys) outKeys->push_back(lf);
                if (outBodies) outBodies->insert(lf.body);
            }
            return;
        }
    }

    // CAMINO LENTO (falta ≥1 hoja fina): partición top-down coarsest-fallback vía emitSubtree, que
    // usa los sets PRECOMPUTADOS (m_leafSet/m_intSet, estáticos entre cambios de hojas → sin
    // reconstruir el trie de internos cada frame). Emitimos cada raíz de cara; el mismo emitSubtree
    // lo reusa el delta-splice para re-emitir solo subárboles tocados. Mismo resultado que la
    // referencia (verificado por el banco: OK==).
    std::unordered_set<uint16_t> bodies;
    for (const PlanetChunkKey& lf : leaves) bodies.insert(lf.body);

    auto lsIt = m_leafSet.find(planet);
    auto isIt = m_intSet.find(planet);
    // Fallback: si por alguna ruta no se precomputaron (setDesiredLeaves siempre los construye), los
    // armamos localmente aquí para no divergir nunca del contrato de la función.
    std::unordered_set<uint64_t> leafLocal, intLocal;
    const std::unordered_set<uint64_t>* leafSet;
    const std::unordered_set<uint64_t>* intSet;
    if (lsIt != m_leafSet.end() && isIt != m_intSet.end()) {
        leafSet = &lsIt->second; intSet = &isIt->second;
    } else {
        leafLocal.reserve(leaves.size()); intLocal.reserve(leaves.size() * 2);
        for (const PlanetChunkKey& lf : leaves) {
            leafLocal.insert(H(lf));
            PlanetChunkKey a = lf;
            while (a.lod > 0) { a = parent(a); if (!intLocal.insert(H(a)).second) break; }
        }
        leafSet = &leafLocal; intSet = &intLocal;
    }

    std::unordered_map<uint64_t,uint8_t> memo; // canCover memoizado, compartido entre raíces
    memo.reserve(leaves.size() * 2);
    for (uint16_t bd : bodies)
        for (int f = 0; f < 6; ++f)
            emitSubtree(*leafSet, *intSet, {(PlanetFace)f, 0, 0, 0, bd}, outHashes, outKeys, memo);
    if (outBodies) *outBodies = std::move(bodies);
}

// Emite (partición top-down coarsest-fallback) el subárbol de `root`, añadiendo los nodos DIBUJADOS
// a outHashes/outKeys. Usa los sets PRECOMPUTADOS (leaf/int) → no reconstruye el trie. `memo` cachea
// canCover (bits DONE/COV) entre llamadas. Es el núcleo COMPARTIDO por buildDrawSet (todas las raíces)
// y applyDrawSetDelta (solo los subárboles tocados). emit(root) depende SOLO de la residencia bajo
// root → re-emitirlo aislado es correcto-por-construcción (las celdas del quadtree anidan o son
// disjuntas). Sin std::function (recursión estática por parámetro self → mejor inlining).
void TerrainRenderer::emitSubtree(const std::unordered_set<uint64_t>& leafSet,
                                  const std::unordered_set<uint64_t>& intSet,
                                  const PlanetChunkKey& root,
                                  std::unordered_set<uint64_t>& outHashes,
                                  std::vector<PlanetChunkKey>* outKeys,
                                  std::unordered_map<uint64_t, uint8_t>& memo) const {
    static constexpr uint8_t DONE = 2, COV = 4;
    auto H    = [](const PlanetChunkKey& k){ return ChunkCache::keyToHash(k); };
    auto kids = [](const PlanetChunkKey& n, PlanetChunkKey c[4]){
        const uint8_t cl=(uint8_t)(n.lod+1); const uint32_t bx=n.x*2, by=n.y*2; const uint16_t bd=n.body;
        c[0]={n.face,cl,bx,by,bd};   c[1]={n.face,cl,bx+1,by,bd};
        c[2]={n.face,cl,bx,by+1,bd}; c[3]={n.face,cl,bx+1,by+1,bd};
    };
    auto residentReady = [&](const PlanetChunkKey& k)->bool{
        auto it = m_gpuMeshes.find(H(k));
        return it != m_gpuMeshes.end() && it->second.isReady;
    };
    // canCover(n): ¿el área de n la cubre ALGÚN residente (a cualquier resolución)? residente basta;
    // interno no-residente → todos los hijos deben cubrir; hoja/nodo desconocido no-residente → hueco.
    auto canCover = [&](auto&& self, const PlanetChunkKey& n)->bool{
        const uint64_t h = H(n);
        auto it = memo.find(h);
        if (it != memo.end() && (it->second & DONE)) return (it->second & COV) != 0;
        bool res;
        if (residentReady(n))       res = true;
        else if (intSet.count(h)) { PlanetChunkKey c[4]; kids(n,c);
                                    res = self(self,c[0]) && self(self,c[1]) && self(self,c[2]) && self(self,c[3]); }
        else                        res = false;
        memo[h] = (uint8_t)(DONE | (res ? COV : 0));
        return res;
    };
    auto emit = [&](auto&& self, const PlanetChunkKey& n)->void{
        if (!canCover(canCover, n)) return;                   // hueco real: nada residente en esta área
        const uint64_t h = H(n);
        if (intSet.count(h)) {                                // interno → ¿subdividir?
            PlanetChunkKey c[4]; kids(n,c);
            if (canCover(canCover,c[0]) && canCover(canCover,c[1]) &&
                canCover(canCover,c[2]) && canCover(canCover,c[3])) {
                self(self,c[0]); self(self,c[1]); self(self,c[2]); self(self,c[3]); return;
            }
        }
        // canCover(n) && (hoja || algún hijo sin cobertura) ⟹ residente(n) → se dibuja n
        (void)leafSet;
        if (outHashes.insert(h).second && outKeys) outKeys->push_back(n);
    };
    emit(emit, root);
}

// Aplica por SPLICE los cambios de residencia pendientes al draw-set cacheado, en vez de reconstruir
// todo. Devuelve false si conviene el rebuild completo (delta grande, sin ancestro residente, o falta
// el cache/sets). Correcto-por-construcción: para cada nodo cambiado K se re-emite el subárbol MÍNIMO
// que puede cambiar, y como emit(R) depende solo de la residencia bajo R (celdas del quadtree anidan
// o son disjuntas) sustituir las celdas viejas de R por emit(R) preserva EXACTO la partición.
//   Re-raíz R por nodo cambiado K (con la residencia YA aplicada en m_gpuMeshes). A = nodo DIBUJADO
//   que cubre K (ÚNICO por partición: subir desde K, primer dibujado). Con la raíz residente (nuestra
//   invariante) SIEMPRE existe.
//     • A más GRUESO que K (A.lod < K.lod): re-emitir A. ADD → puede subdividir hacia K; REMOVE bajo
//       un A grueso = no-op (A sigue grueso) pero re-emitir A es seguro (mismo A). R = A.
//     • A == K (K es una celda dibujada):
//         – K residente (refresco/ADD de esa celda): R = K (puede subdividir).
//         – K NO residente (REMOVE de la celda): R = ancestro RESIDENTE más cercano (la re-partición
//           propaga hacia arriba y para en el primer residente). Sin él → rebuild completo.
//   Así R es SIEMPRE un nodo dibujado (A) o un ancestro residente (B) → nunca dibuja en un hueco
//   (evita divergir de la referencia).
bool TerrainRenderer::applyDrawSetDelta(const std::string& planet) {
    // CALLER HOLDS m_renderMutex.
    static const bool kNoDelta = (getenv("HARUKA_NO_DELTA") != nullptr); // diagnóstico: fuerza rebuild completo
    if (kNoDelta) return false;
    auto pdIt = m_pendingDelta.find(planet);
    if (pdIt == m_pendingDelta.end() || pdIt->second.empty()) return true; // nada que aplicar
    auto& pending = pdIt->second;
    if (pending.size() > kMaxDeltaSplice) return false;                    // demasiado → rebuild completo

    auto cacheIt = m_drawSetCache.find(planet);
    if (cacheIt == m_drawSetCache.end()) return false;                     // sin base → rebuild
    auto lsIt = m_leafSet.find(planet); auto isIt = m_intSet.find(planet);
    if (lsIt == m_leafSet.end() || isIt == m_intSet.end()) return false;
    std::unordered_set<uint64_t>& cache = cacheIt->second;
    const std::unordered_set<uint64_t>& leafSet = lsIt->second;
    const std::unordered_set<uint64_t>& intSet  = isIt->second;

    auto residentReady = [&](const PlanetChunkKey& k)->bool{
        auto it = m_gpuMeshes.find(ChunkCache::keyToHash(k));
        return it != m_gpuMeshes.end() && it->second.isReady;
    };

    // 1) Re-raíces (dedup por conjunto).
    std::unordered_set<uint64_t> rootHashes;
    for (uint64_t h : pending) {
        PlanetChunkKey K = hashToKey(h);
        // A = nodo DIBUJADO que cubre K (subir desde K, primer dibujado del cache).
        PlanetChunkKey A{}; bool haveA = false;
        for (int L = (int)K.lod; L >= 0 && !haveA; --L) {
            PlanetChunkKey anc = ancestorAtLod(K, (uint8_t)L);
            if (cache.count(ChunkCache::keyToHash(anc))) { A = anc; haveA = true; }
        }
        if (!haveA) return false;                                         // sin cobertura dibujada → rebuild
        PlanetChunkKey R;
        if (A.lod < K.lod) {
            R = A;                                                        // K bajo una celda gruesa dibujada
        } else if (residentReady(K)) {
            R = K;                                                        // celda dibujada K residente → refinar
        } else {
            // REMOVE de la celda dibujada K → ancestro residente más cercano (propagación hacia arriba).
            bool haveB = false;
            for (int L = (int)K.lod - 1; L >= 0 && !haveB; --L) {
                PlanetChunkKey anc = ancestorAtLod(K, (uint8_t)L);
                if (residentReady(anc)) { R = anc; haveB = true; }
            }
            if (!haveB) return false;                                     // sin fallback residente → rebuild
        }
        rootHashes.insert(ChunkCache::keyToHash(R));
    }

    // Dedup: si una re-raíz es descendiente de otra, la ancestro ya la cubre → descartar la fina.
    std::vector<PlanetChunkKey> roots;
    roots.reserve(rootHashes.size());
    for (uint64_t rh : rootHashes) {
        PlanetChunkKey R = hashToKey(rh);
        bool coveredByAncestor = false;
        for (int L = (int)R.lod - 1; L >= 0 && !coveredByAncestor; --L)
            if (rootHashes.count(ChunkCache::keyToHash(ancestorAtLod(R, (uint8_t)L)))) coveredByAncestor = true;
        if (!coveredByAncestor) roots.push_back(R);
    }

    // 2) Quitar del cache todas las celdas dibujadas dentro del área de alguna re-raíz.
    std::vector<uint64_t> remove;
    for (uint64_t h : cache) {
        PlanetChunkKey k = hashToKey(h);
        for (const PlanetChunkKey& R : roots)
            if (isAncestorOrEqual(R, k)) { remove.push_back(h); break; }
    }
    for (uint64_t h : remove) cache.erase(h);

    // 3) Re-emitir cada re-raíz (partición coarsest-fallback) sobre la residencia ACTUAL.
    std::unordered_map<uint64_t, uint8_t> memo;
    for (const PlanetChunkKey& R : roots)
        emitSubtree(leafSet, intSet, R, cache, nullptr, memo);

    pending.clear();
    return true;
}

// Algoritmo de REFERENCIA (el original) — lo usa el banco para verificar que buildDrawSet produce
// EXACTAMENTE el mismo conjunto. NO se usa en el render (más lento: dos sets hash + std::function).
void TerrainRenderer::buildDrawSetReference(const std::string& planet,
                                            std::unordered_set<uint64_t>& outHashes,
                                            std::vector<PlanetChunkKey>* outKeys,
                                            std::unordered_set<uint16_t>* outBodies) const {
    auto dit = m_desiredLeaves.find(planet);
    if (dit == m_desiredLeaves.end() || dit->second.empty()) return;
    const auto& leaves = dit->second;

    auto H      = [](const PlanetChunkKey& k){ return ChunkCache::keyToHash(k); };
    auto parent = [](const PlanetChunkKey& k){ return PlanetChunkKey{k.face,(uint8_t)(k.lod-1),k.x>>1,k.y>>1,k.body}; };
    auto kids   = [](const PlanetChunkKey& n, PlanetChunkKey c[4]){
        const uint8_t cl=(uint8_t)(n.lod+1); const uint32_t bx=n.x*2, by=n.y*2; const uint16_t bd=n.body;
        c[0]={n.face,cl,bx,by,bd};   c[1]={n.face,cl,bx+1,by,bd};
        c[2]={n.face,cl,bx,by+1,bd}; c[3]={n.face,cl,bx+1,by+1,bd};
    };
    auto residentReady = [&](const PlanetChunkKey& k)->bool{
        auto it = m_gpuMeshes.find(H(k));
        return it != m_gpuMeshes.end() && it->second.isReady;
    };
    {
        bool allResident = true;
        for (const PlanetChunkKey& lf : leaves) if (!residentReady(lf)) { allResident = false; break; }
        if (allResident) {
            outHashes.reserve(leaves.size());
            for (const PlanetChunkKey& lf : leaves) {
                if (outHashes.insert(H(lf)).second && outKeys) outKeys->push_back(lf);
                if (outBodies) outBodies->insert(lf.body);
            }
            return;
        }
    }
    std::unordered_set<uint64_t> internal;
    std::unordered_set<uint16_t> bodies;
    internal.reserve(leaves.size() * 2);
    for (const PlanetChunkKey& lf : leaves) {
        bodies.insert(lf.body);
        PlanetChunkKey a = lf;
        while (a.lod > 0) { a = parent(a); internal.insert(H(a)); }
    }
    std::unordered_map<uint64_t,char> memo;
    std::function<bool(const PlanetChunkKey&)> canCover = [&](const PlanetChunkKey& n)->bool{
        const uint64_t h = H(n);
        auto m = memo.find(h); if (m != memo.end()) return m->second != 0;
        bool res;
        if (residentReady(n))        res = true;
        else if (internal.count(h)) { PlanetChunkKey c[4]; kids(n,c);
                                      res = canCover(c[0]) && canCover(c[1]) && canCover(c[2]) && canCover(c[3]); }
        else                         res = false;
        memo[h] = res ? 1 : 0; return res;
    };
    std::function<void(const PlanetChunkKey&)> emit = [&](const PlanetChunkKey& n){
        if (!canCover(n)) return;
        if (internal.count(H(n))) {
            PlanetChunkKey c[4]; kids(n,c);
            if (canCover(c[0]) && canCover(c[1]) && canCover(c[2]) && canCover(c[3])) {
                emit(c[0]); emit(c[1]); emit(c[2]); emit(c[3]); return;
            }
        }
        const uint64_t h = H(n);
        if (outHashes.insert(h).second && outKeys) outKeys->push_back(n);
    };
    for (uint16_t bd : bodies)
        for (int f = 0; f < 6; ++f) emit({(PlanetFace)f, 0, 0, 0, bd});
    if (outBodies) *outBodies = std::move(bodies);
}

TerrainRenderer::LODReport TerrainRenderer::validateCoverage() const {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    LODReport r;

    auto H = [](const PlanetChunkKey& k) { return ChunkCache::keyToHash(k); };
    auto residentReady = [&](const PlanetChunkKey& k) -> bool {
        auto it = m_gpuMeshes.find(H(k));
        return it != m_gpuMeshes.end() && it->second.isReady;
    };

    // 1. Conjunto DIBUJADO = MISMA selección que renderPlanet (buildDrawSet, partición top-down).
    //    SIN culling (valida la teselación completa, incluido el lado oculto).
    std::unordered_set<uint64_t> drawn;
    std::vector<PlanetChunkKey>  drawnKeys;
    std::unordered_set<uint16_t> bodies;
    for (const auto& [planet, leaves] : m_desiredLeaves) {
        std::unordered_set<uint16_t> b;
        buildDrawSet(planet, drawn, &drawnKeys, &b);
        bodies.insert(b.begin(), b.end());
    }
    (void)residentReady;
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
    // Pooled: devuelve el slot al free-list, NO borres los buffers compartidos del pool.
    if (mesh.poolSlot >= 0) {
        auto it = m_pools.find(mesh.poolKey);
        if (it != m_pools.end()) it->second.freeSlots.push_back((uint32_t)mesh.poolSlot);
        mesh.poolSlot = -1;
        return;
    }
    if (mesh.vao) glDeleteVertexArrays(1, &mesh.vao);
    if (mesh.vbo) glDeleteBuffers(1, &mesh.vbo);
    if (mesh.mbo) glDeleteBuffers(1, &mesh.mbo);   // antes NO se borraba → fuga de buffer GPU
    if (mesh.nbo) glDeleteBuffers(1, &mesh.nbo);
    if (mesh.nmbo) glDeleteBuffers(1, &mesh.nmbo);
    if (mesh.uvo) glDeleteBuffers(1, &mesh.uvo);
    // mesh.ebo NO se borra: es el EBO COMPARTIDO por res (lo libera el destructor).
}

// --- BATCHING: gestión del pool ------------------------------------------------------
void TerrainRenderer::setupPoolVAO(ChunkPool& p) {
    // (Re)liga los atributos del VAO a los buffers actuales del pool. Debe rellamarse tras
    // recrear buffers (growPool) porque glVertexAttribPointer captura el buffer ligado AHORA.
    glBindVertexArray(p.vao);
    glBindBuffer(GL_ARRAY_BUFFER, p.posVBO);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr); glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, p.morphVBO);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr); glEnableVertexAttribArray(3);
    glBindBuffer(GL_ARRAY_BUFFER, p.normVBO);
    glVertexAttribPointer(1, 4, GL_INT_2_10_10_10_REV, GL_TRUE, sizeof(uint32_t), nullptr); glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, p.morphNormVBO);
    glVertexAttribPointer(4, 4, GL_INT_2_10_10_10_REV, GL_TRUE, sizeof(uint32_t), nullptr); glEnableVertexAttribArray(4);
    glBindBuffer(GL_ARRAY_BUFFER, p.uvVBO);
    glVertexAttribPointer(2, 2, GL_HALF_FLOAT, GL_FALSE, sizeof(uint32_t), nullptr); glEnableVertexAttribArray(2);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p.ebo); // el VAO recuerda este EBO
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

TerrainRenderer::ChunkPool& TerrainRenderer::getOrCreatePool(uint32_t vertexCount, uint32_t indexCount) {
    auto it = m_pools.find(vertexCount);
    if (it != m_pools.end()) return it->second;

    ChunkPool p;
    p.vertexCount = vertexCount;
    p.indexCount  = indexCount;
    // BUFFER FIJO: reservamos UNA vez a la capacidad máxima real y NO volvemos a reasignar nunca. Antes
    // la capacidad nacía en 1024 y crecía por DUPLICACIÓN (growPool = glBufferData nuevo + copia + delete):
    // cada duplicación reorpheaba el pool ENTERO (cientos de MB→GB), un `glBufferData` síncrono en el hilo
    // de render = stall de cientos de ms = el "terreno desaparece al girar/mirar mar" (RenderDoc 22: 2.76 GB
    // subidos en 1 frame con GPU ociosa). Con capacidad fija: 1 sola reserva acotada, cero realloc en runtime,
    // VRAM plana. Los chunks entran solo por glBufferSubData (ya era así).
    //
    // BLINDAJE MULTI-TOPOLOGÍA: el pool DOMINANTE (el primero, la rejilla CDLOD uniforme bajo el jugador —
    // kMaxResidentChunks acota el total residente, cabe entero) recibe la capacidad completa; los pools
    // SECUNDARIOS (otro planeta con distinto chunkSize, islas, cuevas) reciben kSecondaryPoolSlots. Así la
    // VRAM NO se multiplica por nº de vertexCount distintos (era el agujero: N topologías × 12000 slots ≈
    // N×2GB). Si una topología secundaria se vuelve dominante (aterrizas ahí) y llena su pool, growPool
    // dispara (raro) y lo registra en m_poolGrowCount. Vigilar #pools con HARUKA_POOL_DBG.
    p.capacity    = m_pools.empty() ? (uint32_t)kMaxResidentChunks : kSecondaryPoolSlots;
    glGenVertexArrays(1, &p.vao);
    const size_t n = (size_t)vertexCount * p.capacity;
    auto mkbuf = [&](GLuint& b, size_t bytes) {
        glGenBuffers(1, &b);
        glBindBuffer(GL_ARRAY_BUFFER, b);
        glBufferData(GL_ARRAY_BUFFER, bytes, nullptr, GL_DYNAMIC_DRAW); // datos por slot vía glBufferSubData
    };
    mkbuf(p.posVBO,       n * sizeof(glm::vec3));
    mkbuf(p.morphVBO,     n * sizeof(glm::vec3));
    mkbuf(p.normVBO,      n * sizeof(uint32_t));
    mkbuf(p.morphNormVBO, n * sizeof(uint32_t));
    mkbuf(p.uvVBO,        n * sizeof(uint32_t));

    // EBO compartido por indexCount (reusa el de legacy si ya existe). canPool ya garantizó
    // que la tabla compartida tiene esta topología → el buffer se rellena con datos válidos.
    {
        auto eit = m_sharedEBO.find(indexCount);
        if (eit == m_sharedEBO.end()) {
            const std::vector<unsigned int>* idx = SharedIndexTable::get().find(indexCount);
            GLuint ebo = 0;
            glGenBuffers(1, &ebo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, indexCount * sizeof(unsigned int),
                         idx ? idx->data() : nullptr, GL_STATIC_DRAW);
            eit = m_sharedEBO.emplace(indexCount, ebo).first;
        }
        p.ebo = eit->second;
    }
    setupPoolVAO(p);

    p.freeSlots.reserve(p.capacity);
    for (uint32_t s = p.capacity; s-- > 0; ) p.freeSlots.push_back(s); // slot 0 al final → se usa primero
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (getenv("HARUKA_POOL_DBG")) {
        size_t bytesPos = (size_t)vertexCount * p.capacity * sizeof(glm::vec3);
        fprintf(stderr, "[POOL-NEW] vc=%u ic=%u cap=%u  pos=%.1fMB total5=%.1fMB  #pools=%zu\n",
                vertexCount, indexCount, p.capacity, bytesPos/1048576.0,
                (size_t)vertexCount*p.capacity*(12+12+4+4+4)/1048576.0, m_pools.size()+1);
    }
    return m_pools.emplace(vertexCount, std::move(p)).first->second;
}

void TerrainRenderer::growPool(ChunkPool& p) {
    const uint32_t oldCap = p.capacity;
    const uint32_t newCap = oldCap * 2;
    const size_t   vc     = p.vertexCount;
    // VÁLVULA DE SEGURIDAD: con capacidad fija esto NO debería dispararse en el caso normal (1
    // topología dominante ≤ kMaxResidentChunks). Si dispara = una topología secundaria se volvió
    // dominante (transición de planeta) o la suposición se rompió → un realloc gigante = REGRESIÓN
    // del stall que arreglamos. Lo contamos y GRITAMOS siempre (no solo con env var) para detectarlo.
    ++m_poolGrowCount;
    fprintf(stderr, "[POOL-GROW!] vc=%zu %u->%u total=%.1fMB — realloc GIGANTE en hilo de render "
                    "(posible hitch). growCount=%llu. Si es recurrente, subir la capacidad de este "
                    "pool o revisar la suposición de topología única.\n",
            vc, oldCap, newCap, (double)((size_t)vc*newCap*kPoolBytesPerVertex)/1048576.0,
            (unsigned long long)m_poolGrowCount);
    auto regrow = [&](GLuint& buf, size_t elem) {
        GLuint nb = 0;
        glGenBuffers(1, &nb);
        glBindBuffer(GL_ARRAY_BUFFER, nb);
        glBufferData(GL_ARRAY_BUFFER, (size_t)newCap * vc * elem, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_COPY_READ_BUFFER,  buf);   // preserva los slots ya subidos (copia GPU→GPU)
        glBindBuffer(GL_COPY_WRITE_BUFFER, nb);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, (size_t)oldCap * vc * elem);
        glDeleteBuffers(1, &buf);
        buf = nb;
    };
    regrow(p.posVBO,       sizeof(glm::vec3));
    regrow(p.morphVBO,     sizeof(glm::vec3));
    regrow(p.normVBO,      sizeof(uint32_t));
    regrow(p.morphNormVBO, sizeof(uint32_t));
    regrow(p.uvVBO,        sizeof(uint32_t));
    p.capacity = newCap;
    setupPoolVAO(p);                               // religa atributos a los buffers nuevos
    for (uint32_t s = newCap; s-- > oldCap; ) p.freeSlots.push_back(s);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
}

TerrainRenderer::PoolStats TerrainRenderer::poolStats() const {
    std::lock_guard<std::mutex> lk(m_renderMutex);
    PoolStats st;
    st.poolCount = (int)m_pools.size();
    st.growCount = m_poolGrowCount;
    for (const auto& [vc, p] : m_pools) {
        const size_t slots = p.capacity;
        st.totalSlots += slots;
        st.usedSlots  += slots - p.freeSlots.size();
        st.vramBytes  += (size_t)p.capacity * p.vertexCount * kPoolBytesPerVertex;
        if (p.capacity > st.maxPoolCap) st.maxPoolCap = p.capacity;
    }
    return st;
}

} // namespace Haruka