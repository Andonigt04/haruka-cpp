#include <cmath>
#include "water_renderer.h"
#include "tools/profiler.h"
#include "terrain_renderer.h" // drawnHashesFor: sincroniza el agua con el set dibujado del terreno
#include <cstdio>

namespace Haruka {

WaterRenderer::~WaterRenderer() {
    for (auto& pair : m_gpuMeshes) cleanupMesh(pair.second);
    for (auto& [vc, p] : m_pools) {
        if (p.vao) glDeleteVertexArrays(1, &p.vao);
        GLuint bufs[5] = { p.posVBO, p.normVBO, p.paramVBO, p.morphVBO, p.ebo };
        glDeleteBuffers(5, bufs);
        if (p.cmdBuf)   glDeleteBuffers(1, &p.cmdBuf);
        if (p.drawSSBO) glDeleteBuffers(1, &p.drawSSBO);
    }
    if (m_oceanVao) glDeleteVertexArrays(1, &m_oceanVao);
    GLuint ob[5] = { m_oceanPos, m_oceanNorm, m_oceanParam, m_oceanMorph, m_oceanEbo };
    glDeleteBuffers(5, ob);
}

// --- BATCHING: pool de agua ----------------------------------------------------------
void WaterRenderer::setupPoolVAO(WaterPool& p) {
    glBindVertexArray(p.vao);
    glBindBuffer(GL_ARRAY_BUFFER, p.posVBO);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr); glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, p.normVBO);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr); glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, p.paramVBO);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(glm::vec2), nullptr); glEnableVertexAttribArray(2);
    glBindBuffer(GL_ARRAY_BUFFER, p.morphVBO);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr); glEnableVertexAttribArray(3);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p.ebo);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

WaterRenderer::WaterPool& WaterRenderer::getOrCreatePool(uint32_t vertexCount, uint32_t maxIndexCount) {
    auto it = m_pools.find(vertexCount);
    if (it != m_pools.end()) return it->second;
    WaterPool p;
    p.vertexCount   = vertexCount;
    p.maxIndexCount = maxIndexCount;
    p.capacity      = 512;                       // slots iniciales (crece x2)
    glGenVertexArrays(1, &p.vao);
    const size_t nv = (size_t)vertexCount * p.capacity;
    auto mkbuf = [&](GLuint& b, size_t bytes) {
        glGenBuffers(1, &b);
        glBindBuffer(GL_ARRAY_BUFFER, b);
        glBufferData(GL_ARRAY_BUFFER, bytes, nullptr, GL_DYNAMIC_DRAW);
    };
    mkbuf(p.posVBO,   nv * sizeof(glm::vec3));
    mkbuf(p.normVBO,  nv * sizeof(glm::vec3));
    mkbuf(p.paramVBO, nv * sizeof(glm::vec2));
    mkbuf(p.morphVBO, nv * sizeof(glm::vec3));
    glGenBuffers(1, &p.ebo);                      // EBO: maxIndexCount por slot (índices variables)
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (size_t)maxIndexCount * p.capacity * sizeof(unsigned int), nullptr, GL_DYNAMIC_DRAW);
    setupPoolVAO(p);
    p.freeSlots.reserve(p.capacity);
    for (uint32_t s = p.capacity; s-- > 0; ) p.freeSlots.push_back(s);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return m_pools.emplace(vertexCount, std::move(p)).first->second;
}

void WaterRenderer::growPool(WaterPool& p) {
    const uint32_t oldCap = p.capacity, newCap = oldCap * 2;
    const size_t vc = p.vertexCount;
    auto regrow = [&](GLuint& buf, size_t elemPerSlot, size_t elemSize) {
        GLuint nb = 0; glGenBuffers(1, &nb);
        glBindBuffer(GL_ARRAY_BUFFER, nb);
        glBufferData(GL_ARRAY_BUFFER, (size_t)newCap * elemPerSlot * elemSize, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_COPY_READ_BUFFER, buf);
        glBindBuffer(GL_COPY_WRITE_BUFFER, nb);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, (size_t)oldCap * elemPerSlot * elemSize);
        glDeleteBuffers(1, &buf); buf = nb;
    };
    regrow(p.posVBO,   vc, sizeof(glm::vec3));
    regrow(p.normVBO,  vc, sizeof(glm::vec3));
    regrow(p.paramVBO, vc, sizeof(glm::vec2));
    regrow(p.morphVBO, vc, sizeof(glm::vec3));
    // EBO: mismo patrón (maxIndexCount por slot).
    {
        GLuint nb = 0; glGenBuffers(1, &nb);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, nb);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (size_t)newCap * p.maxIndexCount * sizeof(unsigned int), nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_COPY_READ_BUFFER, p.ebo);
        glBindBuffer(GL_COPY_WRITE_BUFFER, nb);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, (size_t)oldCap * p.maxIndexCount * sizeof(unsigned int));
        glDeleteBuffers(1, &p.ebo); p.ebo = nb;
    }
    p.capacity = newCap;
    setupPoolVAO(p);
    for (uint32_t s = newCap; s-- > oldCap; ) p.freeSlots.push_back(s);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
}

void WaterRenderer::addToScene(const std::string& planetName, const PlanetChunkKey& key, const ChunkData& data) {
    uint64_t hash = ChunkCache::keyToHash(key);
    if (!data.hasOcean || data.waterVertices.empty() || data.waterIndices.empty()) {
        // Este chunk NO tiene agua (tierra). Lo MEMORIZAMOS: sin esto, el catch-up del agua reintenta
        // CADA pasada (getChunkCopy pesado + addToScene) para cada chunk de tierra de chunksToKeep —la
        // mayoría del mundo— porque nunca queda "residente" → churn perpetuo (era el grueso de
        // lod.catchup ~5ms). Con la marca, el catch-up lo salta. Cota de memoria: se limpia si crece.
        std::lock_guard<std::mutex> lock(m_renderMutex);
        if (m_noWater.size() > 40000) m_noWater.clear();   // cota (el veredicto se recalcula al revisitar)
        m_noWater.insert(hash);
        return;
    }

    std::lock_guard<std::mutex> lock(m_renderMutex);
    m_noWater.erase(hash);   // ahora SÍ tiene agua (regeneración/edición) → deja de estar marcado
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

    // TOPE DURO de mallas de agua residentes (acota RAM/VRAM ante picos). Ver TerrainRenderer.
    if (!m_gpuMeshes.count(hash) && (int)m_gpuMeshes.size() >= 8000) return;

    RenderMesh mesh;
    mesh.indexCount  = (uint32_t)data.waterIndices.size();
    mesh.vertexCount = (uint32_t)data.waterVertices.size();
    mesh.planetName  = planetName;
    mesh.key         = key;
    mesh.chunkCenter = data.chunkCenter;
    mesh.planetRadius = data.planetRadius;
    {
        double nodeSz = data.planetRadius * 2.0 / double(1u << key.lod);
        mesh.cullRadius = (float)(nodeSz * 0.9);
    }

    // --- CAMINO BATCHING: subir al POOL (vértices uniformes; índices al slot, count real) ------
    const uint32_t vc = mesh.vertexCount;
    // Requisitos MÍNIMOS: pos (=vc por definición) y normal. param/morph pueden faltar en algunas
    // mallas (p.ej. océano sin lago) → antes eso las tiraba a legacy per-chunk (el coste CONSTANTE
    // de water.draw: el océano del horizonte). Ahora las SUSTITUIMOS (param→0 = nivel océano;
    // morph→pos = sin morphing) para que TODO entre al pool → MultiDraw.
    const bool canPool = m_batching
        && vc > 0 && mesh.indexCount > 0
        && data.waterVertices.size() == vc
        && data.waterNormals.size()  == vc;
    if (canPool) {
        // maxIndexCount por slot = peor caso (grid completo, todo mojado) = 6·res². res desde vc=(res+1)².
        const uint32_t res    = (uint32_t)std::lround(std::sqrt((double)vc)) - 1u;
        const uint32_t maxIdx = 6u * res * res + 24u * res + 128u; // 6·res² celdas + faldón + margen
        if (mesh.indexCount <= maxIdx) {
            WaterPool& p = getOrCreatePool(vc, maxIdx);
            if (p.freeSlots.empty()) growPool(p);
            uint32_t slot = p.freeSlots.back(); p.freeSlots.pop_back();
            const size_t vbase = (size_t)slot * vc;
            auto sub = [&](GLuint buf, size_t elem, const void* src) {
                glBindBuffer(GL_ARRAY_BUFFER, buf);
                glBufferSubData(GL_ARRAY_BUFFER, vbase * elem, vc * elem, src);
            };
            sub(p.posVBO,   sizeof(glm::vec3), data.waterVertices.data());
            sub(p.normVBO,  sizeof(glm::vec3), data.waterNormals.data());
            if (data.waterParams.size() == vc) sub(p.paramVBO, sizeof(glm::vec2), data.waterParams.data());
            else { std::vector<glm::vec2> z(vc, glm::vec2(0.0f)); sub(p.paramVBO, sizeof(glm::vec2), z.data()); }
            if (data.waterMorphTargets.size() == vc) sub(p.morphVBO, sizeof(glm::vec3), data.waterMorphTargets.data());
            else sub(p.morphVBO, sizeof(glm::vec3), data.waterVertices.data()); // morph=pos → sin morphing
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            // Índices al slot [slot*maxIdx, +indexCount); 0-based (el baseVertex del comando desplaza).
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p.ebo);
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (size_t)slot * p.maxIndexCount * sizeof(unsigned int),
                            mesh.indexCount * sizeof(unsigned int), data.waterIndices.data());
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
            mesh.poolSlot = (int32_t)slot;
            mesh.poolKey  = vc;
            mesh.vao      = 0;
            mesh.isReady  = true;
            m_gpuMeshes[hash] = mesh;
            return;
        }
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

    // Atributo 3: morph target CDLOD (posición en el LOD padre). Si no viene, el
    // atributo queda deshabilitado y el shader no morfa (mix con factor 0 da igual).
    if (!data.waterMorphTargets.empty()) {
        glGenBuffers(1, &mesh.mbo);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.mbo);
        glBufferData(GL_ARRAY_BUFFER, data.waterMorphTargets.size() * sizeof(glm::vec3), data.waterMorphTargets.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
        glEnableVertexAttribArray(3);
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

void WaterRenderer::residentHashes(std::unordered_set<uint64_t>& out) const {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    out.clear();
    out.reserve(m_gpuMeshes.size());
    for (const auto& [h, mesh] : m_gpuMeshes)
        if (mesh.isReady) out.insert(h);
}

void WaterRenderer::noWaterHashes(std::unordered_set<uint64_t>& out) const {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    out = m_noWater;   // copia bajo lock (snapshot para el catch-up sin re-bloquear por chunk)
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
        if (s.body == key.body && s.face == key.face && s.lod > key.lod) {
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

    // COBERTURA PROPIA DEL AGUA: dibuja el nivel de agua MÁS FINO residente de cada zona
    // (como el primer check del terreno, pero entre mallas de AGUA). Así el agua se ve a
    // CUALQUIER distancia donde haya agua cargada — no depende de que el terreno dibuje ahí un
    // nivel que generó agua (a media distancia el terreno grueso infrasamplea el río/lago y se
    // quedaba sin agua). El agua gruesa que asome sobre tierra seca la TAPA el terreno por
    // delante (depth test: el terreno seco está por encima del nivel del mar).
    auto waterResident = [&](const PlanetChunkKey& k) -> bool {
        auto it = m_gpuMeshes.find(ChunkCache::keyToHash(k));
        return it != m_gpuMeshes.end() && it->second.isReady;
    };

    // SELECCIÓN TOP-DOWN sin huecos — la MISMA que el terreno (TerrainRenderer::buildDrawSet),
    // pero con residencia de AGUA. Antes el agua usaba "dibuja todo salvo lo cubierto por 4 hijos"
    // → dejaba HUECOS y desajustes de LOD ("la costa se corta / falta"). Ahora: desde la raíz de
    // cada cara baja si los 4 hijos cubren con agua residente; si no, dibuja el nodo de agua
    // residente de ese cuadrante. Partición exacta de las hojas deseadas (0 huecos, 0 solapes).
    // Donde no hay agua (tierra) simplemente no se dibuja (no hay malla → canCover false).
    // SINCRONIZACIÓN CON EL TERRENO (rápido + sin diamantes): en vez de reconstruir su PROPIA
    // partición del quadtree cada frame (buildDrawSet caro: ~27000 ops hash + recursión = ~5ms),
    // el agua REUSA el set que el terreno YA calculó este frame (drawnHashesFor). Ventajas:
    //  - Coste ~0.1ms (solo lookups) en vez de ~5ms.
    //  - Agua y terreno quedan SIEMPRE al MISMO LOD por chunk → imposible que el agua salga a un
    //    nivel más grueso que el terreno (= la causa de los "diamantes"). Donde el chunk es seco
    //    (sin malla de agua) o su agua aún no subió, simplemente no se dibuja agua (gap transitorio
    //    que el catch-up rellena), en vez de dibujar agua gruesa sobre tierra.
    // El terreno dibuja ANTES que el agua en el frame (mismo hilo) → drawnHashesFor está fresco.
    std::unordered_set<uint64_t> drawnLocal; // fallback si no hay terreno cableado
    const std::unordered_set<uint64_t>* tset = m_terrain ? m_terrain->drawnHashesFor(planet) : nullptr;
    // Si el set viene del terreno YA está culleado (post-cull, solo lo visible) → NO re-cullear (el
    // cull propio del agua es más permisivo y dibujaba trozos fuera de cámara/lejanos). Solo culleamos
    // en el fallback (cuando el agua construye su propio set sin terreno cableado).
    const bool skipCull = (tset != nullptr);
    if (!tset) {
        auto dit = m_desiredLeaves.find(planet);
        if (dit != m_desiredLeaves.end() && !dit->second.empty()) {
            auto H    = [](const PlanetChunkKey& k){ return ChunkCache::keyToHash(k); };
            auto kids = [](const PlanetChunkKey& n, PlanetChunkKey c[4]){
                const uint8_t cl=(uint8_t)(n.lod+1); const uint32_t bx=n.x*2, by=n.y*2; const uint16_t bd=n.body;
                c[0]={n.face,cl,bx,by,bd};   c[1]={n.face,cl,bx+1,by,bd};
                c[2]={n.face,cl,bx,by+1,bd}; c[3]={n.face,cl,bx+1,by+1,bd};
            };
            std::unordered_set<uint64_t> internal;
            std::unordered_set<uint16_t> bodies;
            for (const auto& lf : dit->second) {
                bodies.insert(lf.body);
                PlanetChunkKey a = lf;
                while (a.lod > 0) { a = {a.face,(uint8_t)(a.lod-1),a.x>>1,a.y>>1,a.body}; internal.insert(H(a)); }
            }
            std::unordered_map<uint64_t,char> memo;
            std::function<bool(const PlanetChunkKey&)> canCover = [&](const PlanetChunkKey& n)->bool{
                const uint64_t h=H(n); auto m=memo.find(h); if(m!=memo.end()) return m->second!=0;
                bool res;
                if (waterResident(n))         res=true;
                else if (internal.count(h)) { PlanetChunkKey c[4]; kids(n,c);
                                              res = canCover(c[0])&&canCover(c[1])&&canCover(c[2])&&canCover(c[3]); }
                else                          res=false;
                memo[h]=res?1:0; return res;
            };
            std::function<void(const PlanetChunkKey&)> emit = [&](const PlanetChunkKey& n){
                if (!canCover(n)) return;
                if (internal.count(H(n))) {
                    PlanetChunkKey c[4]; kids(n,c);
                    if (canCover(c[0])&&canCover(c[1])&&canCover(c[2])&&canCover(c[3])) {
                        emit(c[0]); emit(c[1]); emit(c[2]); emit(c[3]); return;
                    }
                }
                drawnLocal.insert(H(n));
            };
            for (uint16_t bd : bodies) for (int f=0; f<6; ++f) emit({(PlanetFace)f,0,0,0,bd});
        }
        tset = &drawnLocal;
    }
    const std::unordered_set<uint64_t>& drawn = *tset;

    const GLint locBatched = 40;
    glUniform1i(locBatched, 0); // por defecto: draws normales
    for (auto& [k, v] : m_scratchCmds)  v.clear();
    for (auto& [k, v] : m_scratchItems) v.clear();
    GLuint boundVao = 0;

    // Sub-scopes de perfil: renderPlanet no tenía ninguno → water.draw era una caja negra de ~3.5ms.
    // Separa (1) el SELECT (CPU: recorrer el draw-set + morph/cull + armar cmd/item por chunk), (2)
    // el UPLOAD del indirecto (glBufferData STREAM×2/pool), (3) el DRAW (glMultiDraw; el reloj CPU
    // aquí incluye stalls de sync del driver → si domina, el cuello es GPU/fragmento, no CPU).
    {
    HARUKA_PROFILE("water.select");
    // Iteramos el DRAW SET (~cientos). Los chunks pooled se ACUMULAN por pool y se emiten con
    // UNA glMultiDrawElementsIndirect (tras el bucle); si batching OFF o legacy, draw inline.
    for (uint64_t hash : drawn) {
        auto mit = m_gpuMeshes.find(hash);
        if (mit == m_gpuMeshes.end()) continue;
        RenderMesh& mesh = mit->second;
        if (!mesh.isReady || mesh.planetName != planet) continue;
        // chunkCenter es PLANET-LOCAL → sumamos el centro ACTUAL (cuerpos móviles).
        glm::dvec3 worldChunkCenter = mesh.chunkCenter + m_planetCenter;
        glm::vec3 offset = glm::vec3(worldChunkCenter - glm::dvec3(cameraPos));

        if (horizonOn && !skipCull) {
            glm::dvec3 chunkFromCenter = mesh.chunkCenter; // ya relativo al centro
            double cl = glm::length(chunkFromCenter);
            if (cl > 1e-6) {
                double cosChunk    = glm::dot(camDir, chunkFromCenter / cl);
                double cosHorizon  = occR / camDist;
                double horizonDist = std::sqrt(camDist*camDist - occR*occR > 0.0
                                               ? camDist*camDist - occR*occR : 0.0);
                double chunkDist   = glm::length(glm::dvec3(offset));
                // MARGEN por tamaño de chunk (MISMO que el terreno) → si no, el agua se recortaba
                // ANTES que el terreno cerca del horizonte/limbo → "el mar falta en un lado".
                double nodeSize  = mesh.planetRadius * 2.0 / double(1u << mesh.key.lod);
                double angMargin = nodeSize / mesh.planetRadius;
                if (cosChunk < cosHorizon - angMargin && chunkDist > horizonDist + nodeSize) continue;
            }
        }

        if (m_cullEnabled && !skipCull) {
            bool inside = true;
            for (int p = 0; p < 6; ++p) {
                float d = planes[p].x*offset.x + planes[p].y*offset.y + planes[p].z*offset.z + planes[p].w;
                if (d < -mesh.cullRadius) { inside = false; break; }
            }
            if (!inside) continue;
        }

        // Morph CDLOD (mismo cálculo que el terreno → agua y terreno transicionan a la vez).
        const double splitFactor = m_splitFactor;
        double dist     = glm::length(glm::dvec3(offset));
        double nodeSize = mesh.planetRadius * 2.0 / double(1u << mesh.key.lod);
        double low      = nodeSize * splitFactor;
        double high     = nodeSize * splitFactor * 2.0;
        double t        = (high > low) ? (dist - low) / (high - low) : 0.0;
        double morph    = (t - 0.6) / 0.4;
        morph = morph < 0.0 ? 0.0 : (morph > 1.0 ? 1.0 : morph);

        if (mesh.poolSlot >= 0 && m_batching) {
            GpuDrawItem it;
            it.offMorph[0]=offset.x; it.offMorph[1]=offset.y; it.offMorph[2]=offset.z; it.offMorph[3]=(float)morph;
            auto pit = m_pools.find(mesh.poolKey);
            if (pit == m_pools.end()) continue;
            GpuDrawCmd cmd;
            cmd.count         = mesh.indexCount;
            cmd.instanceCount = 1;
            cmd.firstIndex    = (uint32_t)((size_t)mesh.poolSlot * pit->second.maxIndexCount);
            cmd.baseVertex    = (uint32_t)((size_t)mesh.poolSlot * mesh.vertexCount);
            cmd.baseInstance  = 0;
            m_scratchCmds[mesh.poolKey].push_back(cmd);
            m_scratchItems[mesh.poolKey].push_back(it);
            continue;
        }

        // Inline (legacy o batching OFF).
        GLuint wantVao = mesh.vao; GLint baseVertex = 0;
        if (mesh.poolSlot >= 0) {
            auto pit = m_pools.find(mesh.poolKey);
            if (pit == m_pools.end()) continue;
            wantVao = pit->second.vao;
            baseVertex = (GLint)((size_t)mesh.poolSlot * mesh.vertexCount);
        }
        glUniform3fv(locOffset, 1, &offset[0]);
        glUniform1f(12, (float)morph);
        if (wantVao != boundVao) { glBindVertexArray(wantVao); boundVao = wantVao; }
        if (mesh.poolSlot >= 0) {
            auto& pool = m_pools[mesh.poolKey];
            glDrawElementsBaseVertex(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT,
                (const void*)((size_t)mesh.poolSlot * pool.maxIndexCount * sizeof(unsigned int)), baseVertex);
        } else {
            glDrawElements(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, nullptr);
        }
    }

    // --- MultiDrawIndirect: una llamada por pool con todos sus chunks de agua visibles ---------
    if (m_batching) {
        glUniform1i(locBatched, 1);
        for (auto& [poolKey, cmds] : m_scratchCmds) {
            if (cmds.empty()) continue;
            auto pit = m_pools.find(poolKey);
            if (pit == m_pools.end()) continue;
            WaterPool& pool = pit->second;
            auto& items = m_scratchItems[poolKey];
            const size_t n = cmds.size();
            if (pool.cmdBuf == 0)   glGenBuffers(1, &pool.cmdBuf);
            if (pool.drawSSBO == 0) glGenBuffers(1, &pool.drawSSBO);
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, pool.cmdBuf);
            glBufferData(GL_DRAW_INDIRECT_BUFFER, n * sizeof(GpuDrawCmd), cmds.data(), GL_STREAM_DRAW);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, pool.drawSSBO);
            glBufferData(GL_SHADER_STORAGE_BUFFER, n * sizeof(GpuDrawItem), items.data(), GL_STREAM_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, pool.drawSSBO);
            glBindVertexArray(pool.vao);
            glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, (GLsizei)n, 0);
        }
        glUniform1i(locBatched, 0);
    }
    glBindVertexArray(0);
    if (cullWas) glEnable(GL_CULL_FACE);
}

void WaterRenderer::renderSingleOcean(const std::string& planet, const Haruka::WorldPos& cameraPos) {
    (void)planet;
    std::lock_guard<std::mutex> lock(m_renderMutex);
    GLint prog = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    if (prog == 0 || m_planetRadius <= 0.0 || !m_hasPlanetCenter) return;

    const glm::dvec3 camd(cameraPos);
    glm::dvec3 up = camd - m_planetCenter;
    double camDist = glm::length(up);
    if (camDist < 1.0) return;
    up /= camDist;
    const double R = m_planetRadius;
    // Extensión de la rejilla (m). Cada vértice se proyecta a la esfera → un offset tangente d cubre
    // un ángulo atan(d/R) desde el subpunto; para alcanzar el horizonte hace falta extent≈horizon
    // (d_horizonte = R·tan(θ_h) = horizon). El tope viejo R·0.4 solo daba ~22° → desde ÓRBITA el mar
    // no llegaba al limbo. Tope R·6 (θ≈80°) → cubre el casquete visible en órbita; cerca de tierra
    // horizon es pequeño y extent queda fino (densidad intacta).
    const double horizon = std::sqrt(std::max(0.0, camDist * camDist - R * R));
    const double extent  = glm::clamp(horizon * 1.25 + 500.0, 3000.0, R * 6.0);
    const glm::dvec3 sub = m_planetCenter + up * R; // sub-punto de la cámara en la esfera del mar
    const glm::dvec3 ref = (std::abs(up.y) < 0.99) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const glm::dvec3 t1 = glm::normalize(glm::cross(up, ref));
    const glm::dvec3 t2 = glm::cross(up, t1);

    const int N = 192, side = N + 1;
    if (m_oceanVao == 0) {
        glGenVertexArrays(1, &m_oceanVao);
        glBindVertexArray(m_oceanVao);
        auto mk = [&](GLuint& b, int loc, int comps){
            glGenBuffers(1, &b); glBindBuffer(GL_ARRAY_BUFFER, b);
            glBufferData(GL_ARRAY_BUFFER, (size_t)side*side*comps*sizeof(float), nullptr, GL_DYNAMIC_DRAW);
            glVertexAttribPointer(loc, comps, GL_FLOAT, GL_FALSE, comps*sizeof(float), nullptr);
            glEnableVertexAttribArray(loc);
        };
        mk(m_oceanPos, 0, 3); mk(m_oceanNorm, 1, 3); mk(m_oceanParam, 2, 2); mk(m_oceanMorph, 3, 3);
        // Índices (fijos): N×N celdas × 2 triángulos.
        std::vector<unsigned int> idx; idx.reserve((size_t)N*N*6);
        for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x) {
            unsigned int a = y*side + x, b = a+1, c = a+side, d = c+1;
            idx.push_back(a); idx.push_back(c); idx.push_back(b);
            idx.push_back(b); idx.push_back(c); idx.push_back(d);
        }
        m_oceanIdxCount = (int)idx.size();
        glGenBuffers(1, &m_oceanEbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_oceanEbo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size()*sizeof(unsigned int), idx.data(), GL_STATIC_DRAW);
        glBindVertexArray(0);
    }

    // Vértices (relativos al sub-punto), densidad hacia el centro (fino cerca del jugador). Buffers
    // SCRATCH reusados entre frames (antes se realocaban 4×37k = ~1.8 MB/frame → churn = el grueso de
    // water.draw). resize() solo crece; el fill sobrescribe.
    const size_t nv = (size_t)side * side;
    m_scPos.resize(nv); m_scNorm.resize(nv); m_scMorph.resize(nv); m_scParam.resize(nv);
    auto& pos = m_scPos; auto& nrm = m_scNorm; auto& morph = m_scMorph; auto& par = m_scParam;

    // ¿Hace falta reconstruir? Solo si la cámara se movió (> 0.5 m) o si el último build tenía el
    // parche de sim activo (sim evolucionando → hay que re-muestrear). Quieto y sin parche = reusar.
    // Umbral GRUESO (25 m): la rejilla se dibuja con offset relativo al sub del BUILD (mundo exacto,
    // la orilla no se mueve), solo su centro de DENSIDAD va ≤25 m por detrás = imperceptible en el mar.
    // Así en órbita/vuelo no reconstruimos 37k vértices cada micro-movimiento (build era ~5 ms).
    const bool camMoved = glm::length(camd - m_ocLastCamd) > 25.0;
    // Rama de fluido SOLO cerca de la superficie: en alto ningún vértice está a <600 m del sub → nos
    // ahorramos 37k length() inútiles. camDist-R = altitud sobre el nivel del mar.
    const bool nearSurface = (camDist - R) < 700.0;
    const bool needRebuild = !m_ocBuilt || camMoved || (m_ocFluidWasActive && nearSurface);
    // nrm (radial) y par (constante) SOLO cambian si la cámara se movió; bajo fluido-quieto solo
    // cambian pos/morph → nos ahorramos 2 de los 4 uploads (~740 KB) cerca de costa cada frame.
    const bool fullUpload = !m_ocBuilt || camMoved;
    bool fluidActive = false;
    if (needRebuild) {
        HARUKA_PROFILE("water.build");
        for (int y = 0; y < side; ++y) for (int x = 0; x < side; ++x) {
            double u = (double)x / N * 2.0 - 1.0, v = (double)y / N * 2.0 - 1.0;
            // Warp con derivada NO nula en 0 (lineal+cuadrático): densidad cerca del centro SIN la fila
            // degenerada de u·|u| (que dejaba un pliegue/costura hacia el horizonte).
            double wu = u * (0.3 + 0.7 * std::abs(u)), wv = v * (0.3 + 0.7 * std::abs(v));
            glm::dvec3 p    = sub + t1 * (wu * extent) + t2 * (wv * extent);
            glm::dvec3 rad  = glm::normalize(p - m_planetCenter);      // dirección radial del vértice
            glm::dvec3 sp   = m_planetCenter + rad * R;                // punto en la esfera del mar
            // F5.1: si el vértice cae en el parche de la sim (SOLO cerca del jugador → gate por distancia,
            // el parche es pequeño), desplaza el nivel radialmente por (superficie_sim − nivel_mar)·mezcla.
            double radius = R;
            if (m_fluidSample && nearSurface) {
                glm::dvec3 d = sp - camd;
                if (glm::dot(d, d) < 600.0 * 600.0) {                 // dist² < 600² (sin sqrt)
                    float delta = 0.0f, blend = 0.0f;
                    if (m_fluidSample(sp, delta, blend)) { radius = R + (double)delta * (double)blend; fluidActive = true; }
                }
            }
            sp = m_planetCenter + rad * radius;
            size_t i = (size_t)x + (size_t)y*side;
            pos[i]   = glm::vec3(sp - sub);
            if (fullUpload) {
                nrm[i] = glm::vec3(rad);                              // radial (WorldDir para la costa per-píxel)
                par[i] = glm::vec2(0.0f, 50.0f);                      // nivel 0 (océano) · sombreado mar abierto
            }
            morph[i] = pos[i];
        }
    }
    if (needRebuild) {
        HARUKA_PROFILE("water.upload");
        glBindBuffer(GL_ARRAY_BUFFER, m_oceanPos);   glBufferSubData(GL_ARRAY_BUFFER, 0, pos.size()*sizeof(glm::vec3), pos.data());
        glBindBuffer(GL_ARRAY_BUFFER, m_oceanMorph); glBufferSubData(GL_ARRAY_BUFFER, 0, morph.size()*sizeof(glm::vec3), morph.data());
        if (fullUpload) {
            glBindBuffer(GL_ARRAY_BUFFER, m_oceanNorm);  glBufferSubData(GL_ARRAY_BUFFER, 0, nrm.size()*sizeof(glm::vec3), nrm.data());
            glBindBuffer(GL_ARRAY_BUFFER, m_oceanParam); glBufferSubData(GL_ARRAY_BUFFER, 0, par.size()*sizeof(glm::vec2), par.data());
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        m_ocLastCamd = camd; m_ocSubBuilt = sub; m_ocBuilt = true; m_ocFluidWasActive = fluidActive;
    }

    HARUKA_PROFILE("water.drawcall");
    // Offset relativo al sub del BUILD (no al actual): entre rebuilds la geometría cacheada (pos rel.
    // a sub_build) queda en su posición MUNDO exacta → la orilla no deriva; solo el centro de densidad lag.
    glm::vec3 offset = glm::vec3(m_ocSubBuilt - camd);
    glUniform3fv(10, 1, &offset[0]);  // u_chunkOffset
    glUniform1f(12, 0.0f);            // u_morphFactor (sin morph)
    glUniform1i(29, 1);               // u_oceanSurface = 1 (recorte per-píxel siempre)
    GLboolean cullWas = glIsEnabled(GL_CULL_FACE); glDisable(GL_CULL_FACE);
    glBindVertexArray(m_oceanVao);
    glDrawElements(GL_TRIANGLES, m_oceanIdxCount, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
    glUniform1i(29, 0);
    if (cullWas) glEnable(GL_CULL_FACE);
}

void WaterRenderer::cleanupMesh(RenderMesh& mesh) {
    if (mesh.poolSlot >= 0) {   // pooled: devuelve el slot, NO borres los buffers compartidos
        auto it = m_pools.find(mesh.poolKey);
        if (it != m_pools.end()) it->second.freeSlots.push_back((uint32_t)mesh.poolSlot);
        mesh.poolSlot = -1;
        return;
    }
    if (mesh.vao) glDeleteVertexArrays(1, &mesh.vao);
    if (mesh.vbo) glDeleteBuffers(1, &mesh.vbo);
    if (mesh.nbo) glDeleteBuffers(1, &mesh.nbo);
    if (mesh.pbo) glDeleteBuffers(1, &mesh.pbo);
    if (mesh.mbo) glDeleteBuffers(1, &mesh.mbo);
    if (mesh.ebo) glDeleteBuffers(1, &mesh.ebo);
}

} // namespace Haruka
