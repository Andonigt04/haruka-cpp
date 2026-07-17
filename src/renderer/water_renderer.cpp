#include <cmath>
#include <glad/glad.h>   // glBindBufferBase del SSBO del campo (escotilla nativa)
#include "water_renderer.h"
#include "tools/profiler.h"
#include "terrain_renderer.h" // drawnHashesFor: sincroniza el agua con el set dibujado del terreno
#include "renderer/shader.h"  // Shader::baseDir (createPipeline NO resuelve rutas)
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"
#include <cstdio>

namespace Haruka {

WaterRenderer::~WaterRenderer() {
    RHI::Device* dev = RHI::device();
    for (auto& pair : m_gpuMeshes) cleanupMesh(pair.second);
    if (!dev) return;
    for (auto& [vc, p] : m_pools) {
        if (RHI::valid(p.hPos)) {
            dev->destroy(p.hPos); dev->destroy(p.hNorm); dev->destroy(p.hParam);
            dev->destroy(p.hMorph); dev->destroy(p.hEbo);
        }
        if (RHI::valid(p.hCmd))  dev->destroy(p.hCmd);
        if (RHI::valid(p.hSSBO)) dev->destroy(p.hSSBO);
    }
    if (RHI::valid(m_hOceanPos)) {
        dev->destroy(m_hOceanPos); dev->destroy(m_hOceanNorm); dev->destroy(m_hOceanParam);
        dev->destroy(m_hOceanMorph); dev->destroy(m_hOceanEbo);
    }
    if (RHI::valid(m_pso))       dev->destroy(m_pso);
    if (RHI::valid(m_uboParams)) dev->destroy(m_uboParams);
    if (RHI::valid(m_soloSSBO))  dev->destroy(m_soloSSBO);
}

// --- PSO del agua: uno solo para los tres caminos (mismo layout de 4 streams) --------------
bool WaterRenderer::ensurePipeline() {
    RHI::Device* dev = RHI::device();
    if (!dev) return false;
    if (RHI::valid(m_pso)) return true;

    const std::string vs = Shader::baseDir() + "shaders/water.vert";
    const std::string fs = Shader::baseDir() + "shaders/water.frag";
    RHI::PipelineDesc pd;
    pd.vertexPath   = vs.c_str();
    pd.fragmentPath = fs.c_str();
    pd.vertexLayout.strides = {
        (uint32_t)sizeof(glm::vec3),  // 0: aPos
        (uint32_t)sizeof(glm::vec3),  // 1: aNormal (radial)
        (uint32_t)sizeof(glm::vec2),  // 2: aWaterParam (nivel, profundidad)
        (uint32_t)sizeof(glm::vec3),  // 3: aMorphTarget
    };
    pd.vertexLayout.attributes = {
        { 0, 0, RHI::Format::RGB32F, 0 },
        { 1, 0, RHI::Format::RGB32F, 1 },
        { 2, 0, RHI::Format::RG32F,  2 },
        { 3, 0, RHI::Format::RGB32F, 3 },
    };
    pd.topology      = RHI::PrimitiveTopology::Triangles;
    pd.depth.test    = true;
    // El agua NO escribe profundidad: depth-TESTA (el terreno delante la ocluye) pero no escribe,
    // para que el TERRENO bajo el agua se siga viendo y no quede tapado por el océano.
    pd.depth.write   = false;
    pd.depth.compare = RHI::CompareOp::GreaterEqual;   // reversed-Z
    pd.blend.enable  = true;
    pd.blend.mode    = RHI::BlendMode::Alpha;
    // A DOS CARAS: la superficie del océano tiene que verse también desde DEBAJO (cámara bajo el
    // agua). Con cull back (el que dejaba el pase de terreno) no se veía nada al sumergirse.
    pd.cull          = RHI::CullMode::None;
    m_pso = dev->createPipeline(pd);

    m_uboParams = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(WaterParamsUBO), nullptr,
                                    RHI::BufferMemory::Dynamic);
    m_soloSSBO  = dev->createBuffer(RHI::BufferUsage::Storage, sizeof(GpuDrawItem), nullptr,
                                    RHI::BufferMemory::Dynamic);

    return RHI::valid(m_pso);
}

void WaterRenderer::setPassParams(const PassParams& p) {
    m_params.waveUpTime  = glm::vec4(p.waveUp,        p.time);
    m_params.waveTanWind = glm::vec4(p.waveTangent,   p.windStrength);
    m_params.waveBitTide = glm::vec4(p.waveBitangent, p.tideHeight);
    m_params.relCamNear   = glm::vec4(p.planetRelCam, p.nearPlane);
    m_params.windDir      = p.windDir;
    m_params.waterQuality = p.waterQuality;
    m_params.fieldRes     = (float)p.fieldRes;
    m_sceneDepth          = p.sceneDepth;
    m_fieldSSBO           = p.fieldSSBO;
}

// Ata pipeline + UBO del pase + la profundidad de la escena. Común a los tres caminos (océano
// único, chunk suelto, pool): ya no hay flag de camino — el recorte de costa lo hace el depth.
RHI::Context* WaterRenderer::beginPass() {
    RHI::Device* dev = RHI::device();
    RHI::Context* ctx = dev->beginFrame();
    ctx->bindPipeline(m_pso);      // depth GEQUAL sin escritura + blend alfa + sin cull
    dev->updateBuffer(m_uboParams, 0, sizeof(WaterParamsUBO), &m_params);
    ctx->bindUniformBuffer(8, m_uboParams);   // 8: el 7 es el SSBO por-draw
    ctx->bindTexture(6, m_sceneDepth);        // (F1) profundidad del terreno bajo cada píxel
    // (F3) El CAMPO del planeta: el agua consulta si aquí HAY agua (si no, existe también bajo tierra).
    if (m_fieldSSBO) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, m_fieldSSBO);
    return ctx;
}

// --- BATCHING: pool de agua ----------------------------------------------------------
// (Sin VAO: el layout vive en el PSO y los buffers se atan por handle → growPool no religa nada.)
WaterRenderer::WaterPool& WaterRenderer::getOrCreatePool(uint32_t vertexCount, uint32_t maxIndexCount) {
    auto it = m_pools.find(vertexCount);
    if (it != m_pools.end()) return it->second;
    WaterPool p;
    p.vertexCount   = vertexCount;
    p.maxIndexCount = maxIndexCount;
    p.capacity      = 512;                       // slots iniciales (crece x2)
    const size_t nv = (size_t)vertexCount * p.capacity;
    RHI::Device* dev = RHI::device();
    auto mkbuf = [&](Haruka::RHI::BufferHandle& h, RHI::BufferUsage usage, size_t bytes) {
        h = dev->createBuffer(usage, bytes, nullptr, RHI::BufferMemory::Dynamic);
    };
    mkbuf(p.hPos,   RHI::BufferUsage::Vertex, nv * sizeof(glm::vec3));
    mkbuf(p.hNorm,  RHI::BufferUsage::Vertex, nv * sizeof(glm::vec3));
    mkbuf(p.hParam, RHI::BufferUsage::Vertex, nv * sizeof(glm::vec2));
    mkbuf(p.hMorph, RHI::BufferUsage::Vertex, nv * sizeof(glm::vec3));
    mkbuf(p.hEbo,   RHI::BufferUsage::Index,  (size_t)maxIndexCount * p.capacity * sizeof(unsigned int));
    // Multidraw: comandos + items, reasignados enteros cada frame (orphan) → Stream.
    p.hCmd  = dev->createBuffer(RHI::BufferUsage::Indirect, 0, nullptr, RHI::BufferMemory::Stream);
    p.hSSBO = dev->createBuffer(RHI::BufferUsage::Storage,  0, nullptr, RHI::BufferMemory::Stream);
    p.freeSlots.reserve(p.capacity);
    for (uint32_t s = p.capacity; s-- > 0; ) p.freeSlots.push_back(s);
    return m_pools.emplace(vertexCount, std::move(p)).first->second;
}

void WaterRenderer::growPool(WaterPool& p) {
    const uint32_t oldCap = p.capacity, newCap = oldCap * 2;
    const size_t vc = p.vertexCount;
    RHI::Device* dev = RHI::device();
    auto regrow = [&](Haruka::RHI::BufferHandle& h, RHI::BufferUsage usage, size_t elemPerSlot, size_t elemSize) {
        const size_t newBytes = (size_t)newCap * elemPerSlot * elemSize;
        const size_t oldBytes = (size_t)oldCap * elemPerSlot * elemSize;
        Haruka::RHI::BufferHandle nh = dev->createBuffer(usage, newBytes, nullptr, RHI::BufferMemory::Dynamic);
        dev->copyBuffer(h, nh, 0, 0, oldBytes);
        dev->destroy(h);
        h = nh;
    };
    regrow(p.hPos,   RHI::BufferUsage::Vertex, vc, sizeof(glm::vec3));
    regrow(p.hNorm,  RHI::BufferUsage::Vertex, vc, sizeof(glm::vec3));
    regrow(p.hParam, RHI::BufferUsage::Vertex, vc, sizeof(glm::vec2));
    regrow(p.hMorph, RHI::BufferUsage::Vertex, vc, sizeof(glm::vec3));
    regrow(p.hEbo,   RHI::BufferUsage::Index,  p.maxIndexCount, sizeof(unsigned int));
    p.capacity = newCap;
    for (uint32_t s = newCap; s-- > oldCap; ) p.freeSlots.push_back(s);
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
            RHI::Device* pdev = RHI::device();
            auto sub = [&](RHI::BufferHandle h, size_t elem, const void* src) {
                pdev->updateBuffer(h, vbase * elem, vc * elem, src);
            };
            sub(p.hPos,  sizeof(glm::vec3), data.waterVertices.data());
            sub(p.hNorm, sizeof(glm::vec3), data.waterNormals.data());
            if (data.waterParams.size() == vc) sub(p.hParam, sizeof(glm::vec2), data.waterParams.data());
            else { std::vector<glm::vec2> z(vc, glm::vec2(0.0f)); sub(p.hParam, sizeof(glm::vec2), z.data()); }
            if (data.waterMorphTargets.size() == vc) sub(p.hMorph, sizeof(glm::vec3), data.waterMorphTargets.data());
            else sub(p.hMorph, sizeof(glm::vec3), data.waterVertices.data()); // morph=pos → sin morphing
            // Índices al slot [slot*maxIdx, +indexCount); 0-based (el baseVertex del comando desplaza).
            pdev->updateBuffer(p.hEbo, (size_t)slot * p.maxIndexCount * sizeof(unsigned int),
                               mesh.indexCount * sizeof(unsigned int), data.waterIndices.data());
            mesh.poolSlot = (int32_t)slot;
            mesh.poolKey  = vc;
            mesh.isReady  = true;
            m_gpuMeshes[hash] = mesh;
            return;
        }
    }

    // --- CAMINO SIN POOL: buffers propios, un stream por binding del PSO ----------------------
    // Los 4 streams se crean SIEMPRE. El PSO tiene un VAO ÚNICO compartido por todos los draws: un
    // binding sin atar seguiría leyendo el buffer del chunk ANTERIOR (basura silenciosa) en vez del
    // constante 0 que daba el VAO-por-malla con el atributo deshabilitado. Los ausentes se
    // sintetizan con su valor neutro (param = 0 → océano; morph = la propia posición → sin morph).
    RHI::Device* dev = RHI::device();
    auto mkStatic = [&](Haruka::RHI::BufferHandle& h, RHI::BufferUsage usage, const void* d, size_t bytes) {
        h = dev->createBuffer(usage, bytes, d);
    };

    mkStatic(mesh.hVbo, RHI::BufferUsage::Vertex, data.waterVertices.data(), (size_t)vc * sizeof(glm::vec3));

    if (data.waterNormals.size() == vc)
        mkStatic(mesh.hNbo, RHI::BufferUsage::Vertex, data.waterNormals.data(), (size_t)vc * sizeof(glm::vec3));
    else { std::vector<glm::vec3> z(vc, glm::vec3(0.0f));
           mkStatic(mesh.hNbo, RHI::BufferUsage::Vertex, z.data(), (size_t)vc * sizeof(glm::vec3)); }

    if (data.waterParams.size() == vc)   // x = nivel (0 = océano), y = profundidad (m)
        mkStatic(mesh.hPbo, RHI::BufferUsage::Vertex, data.waterParams.data(), (size_t)vc * sizeof(glm::vec2));
    else { std::vector<glm::vec2> z(vc, glm::vec2(0.0f));
           mkStatic(mesh.hPbo, RHI::BufferUsage::Vertex, z.data(), (size_t)vc * sizeof(glm::vec2)); }

    if (data.waterMorphTargets.size() == vc)
        mkStatic(mesh.hMbo, RHI::BufferUsage::Vertex, data.waterMorphTargets.data(), (size_t)vc * sizeof(glm::vec3));
    else                                  // morph = la propia posición → mix() es la identidad
        mkStatic(mesh.hMbo, RHI::BufferUsage::Vertex, data.waterVertices.data(), (size_t)vc * sizeof(glm::vec3));

    mkStatic(mesh.hEbo, RHI::BufferUsage::Index, data.waterIndices.data(), data.waterIndices.size() * sizeof(unsigned int));

    mesh.isReady = true;
    m_gpuMeshes[hash] = mesh;
    // El agua NO se autopurga: su borrado lo dirige el TerrainRenderer (callback
    // onChunkRemoved) para que agua y terreno cubran SIEMPRE lo mismo (sin huecos
    // de océano ni mallas de agua huérfanas). Aquí solo reemplazamos si era stale.
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

    if (!ensurePipeline()) return;
    RHI::Device*  dev = RHI::device();
    RHI::Context* ctx = beginPass();

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

    // (A dos caras —visible desde bajo el agua— y depth sin escritura: ahora los hornea el PSO.)

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

    for (auto& [k, v] : m_scratchCmds)  v.clear();
    for (auto& [k, v] : m_scratchItems) v.clear();

    // Sub-scopes de perfil: renderPlanet no tenía ninguno → water.draw era una caja negra de ~3.5ms.
    // Separa (1) el SELECT (CPU: recorrer el draw-set + morph/cull + armar cmd/item por chunk), (2)
    // el UPLOAD del indirecto (orphan ×2/pool), (3) el DRAW (el reloj CPU aquí incluye stalls de
    // sync del driver → si domina, el cuello es GPU/fragmento, no CPU).
    {
    HARUKA_PROFILE("water.select");
    // Iteramos el DRAW SET (~cientos). Los chunks pooled se ACUMULAN por pool y se emiten con UN
    // drawIndexedIndirect (tras el bucle); los que no tienen slot van con un drawIndexed suelto.
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

        // Datos por-draw: SIEMPRE por el SSBO (gl_DrawID), nunca por uniforms.
        GpuDrawItem it;
        it.offMorph[0]=offset.x; it.offMorph[1]=offset.y; it.offMorph[2]=offset.z; it.offMorph[3]=(float)morph;

        if (mesh.poolSlot >= 0) {
            // POOLED: acumular para el multidraw de su pool (gl_DrawID = índice del comando).
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

        // SIN POOL (el DEFAULT del agua: m_batching = false): draw suelto con sus propios buffers.
        // gl_DrawID vale 0 → lee el item 0 del SSBO de UN elemento. Mismo shader, sin ramas.
        dev->updateBuffer(m_soloSSBO, 0, sizeof(GpuDrawItem), &it);
        ctx->bindStorageBuffer(7, m_soloSSBO);
        ctx->bindVertexBuffer(mesh.hVbo, 0);
        ctx->bindVertexBuffer(mesh.hNbo, 1);
        ctx->bindVertexBuffer(mesh.hPbo, 2);
        ctx->bindVertexBuffer(mesh.hMbo, 3);
        ctx->bindIndexBuffer(mesh.hEbo);
        ctx->drawIndexed(mesh.indexCount);
    }
    } // fin scope "water.select"

    // --- MultiDrawIndirect: una llamada por pool con todos sus chunks de agua visibles ---------
    for (auto& [poolKey, cmds] : m_scratchCmds) {
        if (cmds.empty()) continue;
        auto pit = m_pools.find(poolKey);
        if (pit == m_pools.end()) continue;
        WaterPool& pool = pit->second;
        auto& items = m_scratchItems[poolKey];
        const size_t n = cmds.size();
        // Orphan + upload (Stream): el driver da buffers nuevos sin esperar a la multidraw anterior.
        dev->uploadBuffer(pool.hCmd,  n * sizeof(GpuDrawCmd),  cmds.data());
        dev->uploadBuffer(pool.hSSBO, n * sizeof(GpuDrawItem), items.data());
        ctx->bindStorageBuffer(7, pool.hSSBO);
        ctx->bindVertexBuffer(pool.hPos,   0);
        ctx->bindVertexBuffer(pool.hNorm,  1);
        ctx->bindVertexBuffer(pool.hParam, 2);
        ctx->bindVertexBuffer(pool.hMorph, 3);
        ctx->bindIndexBuffer(pool.hEbo);
        ctx->drawIndexedIndirect(pool.hCmd, (uint32_t)n, (uint32_t)sizeof(GpuDrawCmd));
    }
}

void WaterRenderer::renderSingleOcean(const std::string& planet, const Haruka::WorldPos& cameraPos) {
    (void)planet;
    std::lock_guard<std::mutex> lock(m_renderMutex);
    if (m_planetRadius <= 0.0 || !m_hasPlanetCenter) return;
    if (!ensurePipeline()) return;
    RHI::Device* dev = RHI::device();

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
    if (!RHI::valid(m_hOceanPos)) {
        auto mk = [&](Haruka::RHI::BufferHandle& h, int comps){
            size_t bytes = (size_t)side*side*comps*sizeof(float);
            h = dev->createBuffer(RHI::BufferUsage::Vertex, bytes, nullptr, RHI::BufferMemory::Dynamic);
        };
        mk(m_hOceanPos, 3); mk(m_hOceanNorm, 3); mk(m_hOceanParam, 2); mk(m_hOceanMorph, 3);
        // Índices (fijos): N×N celdas × 2 triángulos.
        std::vector<unsigned int> idx; idx.reserve((size_t)N*N*6);
        for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x) {
            unsigned int a = y*side + x, b = a+1, c = a+side, d = c+1;
            idx.push_back(a); idx.push_back(c); idx.push_back(b);
            idx.push_back(b); idx.push_back(c); idx.push_back(d);
        }
        m_oceanIdxCount = (int)idx.size();
        m_hOceanEbo = dev->createBuffer(RHI::BufferUsage::Index, idx.size()*sizeof(unsigned int), idx.data());
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
        dev->updateBuffer(m_hOceanPos,   0, pos.size()*sizeof(glm::vec3),   pos.data());
        dev->updateBuffer(m_hOceanMorph, 0, morph.size()*sizeof(glm::vec3), morph.data());
        if (fullUpload) {
            dev->updateBuffer(m_hOceanNorm,  0, nrm.size()*sizeof(glm::vec3), nrm.data());
            dev->updateBuffer(m_hOceanParam, 0, par.size()*sizeof(glm::vec2), par.data());
        }
        m_ocLastCamd = camd; m_ocSubBuilt = sub; m_ocBuilt = true; m_ocFluidWasActive = fluidActive;
    }

    HARUKA_PROFILE("water.drawcall");
    // Offset relativo al sub del BUILD (no al actual): entre rebuilds la geometría cacheada (pos rel.
    // a sub_build) queda en su posición MUNDO exacta → la orilla no deriva; solo el centro de densidad lag.
    glm::vec3 offset = glm::vec3(m_ocSubBuilt - camd);
    GpuDrawItem it;
    it.offMorph[0]=offset.x; it.offMorph[1]=offset.y; it.offMorph[2]=offset.z;
    it.offMorph[3]=0.0f;                       // sin morph (la rejilla es única, no hay LOD que fundir)
    dev->updateBuffer(m_soloSSBO, 0, sizeof(GpuDrawItem), &it);

    RHI::Context* ctx = beginPass();
    ctx->bindStorageBuffer(7, m_soloSSBO);     // draw suelto → gl_DrawID = 0 → item 0
    ctx->bindVertexBuffer(m_hOceanPos,   0);
    ctx->bindVertexBuffer(m_hOceanNorm,  1);
    ctx->bindVertexBuffer(m_hOceanParam, 2);
    ctx->bindVertexBuffer(m_hOceanMorph, 3);
    ctx->bindIndexBuffer(m_hOceanEbo);
    ctx->drawIndexed((uint32_t)m_oceanIdxCount);
}

void WaterRenderer::cleanupMesh(RenderMesh& mesh) {
    if (mesh.poolSlot >= 0) {   // pooled: devuelve el slot, NO borres los buffers compartidos
        auto it = m_pools.find(mesh.poolKey);
        if (it != m_pools.end()) it->second.freeSlots.push_back((uint32_t)mesh.poolSlot);
        mesh.poolSlot = -1;
        return;
    }
    if (RHI::valid(mesh.hVbo)) {
        if (RHI::Device* dev = RHI::device()) {
            dev->destroy(mesh.hVbo);
            if (RHI::valid(mesh.hNbo)) dev->destroy(mesh.hNbo);
            if (RHI::valid(mesh.hPbo)) dev->destroy(mesh.hPbo);
            if (RHI::valid(mesh.hMbo)) dev->destroy(mesh.hMbo);
            if (RHI::valid(mesh.hEbo)) dev->destroy(mesh.hEbo);
        }
        mesh.hVbo = mesh.hNbo = mesh.hPbo = mesh.hMbo = mesh.hEbo = {};
    }
}

} // namespace Haruka
