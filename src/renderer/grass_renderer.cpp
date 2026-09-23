#include "renderer/grass_renderer.h"

#include "core/logger.h"
#include "world/terrain/cube_sphere.h"   // dirToCubeFaceClosed

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace Haruka::Renderer {

namespace {

// Celdas por lado de cara del cubo: 2^22 → en la Tierra R·π/2/2^22 = 2,39 m de lado. Con 2^24 el
// float de `lx` en el compute (ulp ~6e-8) ya no fija la celda de forma estable; a 2^22 sobran 4 bits.
constexpr int    kCellBits   = 22;
constexpr int    kCellsFace  = 1 << kCellBits;
constexpr int    kMaxStamps  = 64;
constexpr int    kNodesMax   = 512;
// Rejilla del INDICE ESPACIAL de `drawnHeightAt` (ver grass_gen.comp): cubre el disco en coords de
// la cara (lx,ly) con kIdxDim×kIdxDim celdas; por celda entran todos los nodos que la tocan
// (tope kNodesMax: peor caso = un nodo grueso en todas las celdas).
constexpr int    kIdxDim     = 16;

bool s_warnMatUBO = false;

struct GenUBO {
    glm::vec4  center, centerLo, camFwd;
    glm::ivec4 cells;
    glm::vec4  grid, lod;
    glm::ivec4 nodeGrid;
    glm::vec4  misc;
    glm::vec4  plane[4];   ///< los 4 planos laterales del frustum (normal hacia DENTRO, por el ojo)
    glm::dvec4 uIdx;       ///< x,y = esquina min de la rejilla (lx,ly) · z = lado de celda (lx) · w = lado total
    glm::ivec4 uIdxGrid;   ///< x,y = dims de la rejilla (celdas) · z,w = (libre)
};
struct DrawUBO {
    glm::mat4 mvp;
    glm::vec4 wind, sun, pressA, pressE, pressN, shade, aerial, up;
    glm::vec4 camShift;   ///< camara del ultimo dispatch − camara actual: ancla el campo al mundo
};
struct PressUBO {
    glm::vec4 shift, decay;
    glm::vec4 stamp[kMaxStamps];
    glm::vec4 stampDir[kMaxStamps];
};

} // namespace

bool GrassRenderer::enabled() {
    static const bool s_on = [] { const char* e = std::getenv("HARUKA_GRASS"); return !(e && e[0] == '0'); }();
    return s_on;
}

bool GrassRenderer::init(RHI::Device* dev, const std::string& shaderDir, const Config& cfg) {
    m_dev = dev; m_cfg = cfg;
    if (!dev) return false;
    if (const char* e = std::getenv("HARUKA_GRASS_DENSITY")) m_cfg.density = std::clamp((float)std::atof(e), 0.0f, 1.0f);
    if (const char* e = std::getenv("HARUKA_GRASS_RADIUS"))  m_cfg.radiusM = std::clamp((float)std::atof(e), 5.0f, 200.0f);

    const std::string gen = shaderDir + "grass_gen.comp";
    const std::string vs  = shaderDir + "grass.vert",       fs  = shaderDir + "grass.frag";
    const std::string pvs = shaderDir + "grass_press.vert", pfs = shaderDir + "grass_press.frag";
    { RHI::PipelineDesc pd; pd.computePath = gen.c_str(); m_genPipe = dev->createPipeline(pd); }
    {
        RHI::PipelineDesc pd;
        pd.vertexPath = vs.c_str(); pd.fragmentPath = fs.c_str();
        pd.topology = RHI::PrimitiveTopology::Triangles;
        pd.depth.test = true; pd.depth.write = true;
        pd.blend.enable = false;
        pd.cull = RHI::CullMode::None;          // dos caras: una brizna se ve por delante y por detras
        m_drawPipe = dev->createPipeline(pd);
    }
    {
        RHI::PipelineDesc pd;
        pd.vertexPath = pvs.c_str(); pd.fragmentPath = pfs.c_str();
        pd.topology = RHI::PrimitiveTopology::Triangles;
        pd.depth.test = false; pd.depth.write = false;
        pd.blend.enable = false;
        m_pressPipe = dev->createPipeline(pd);
    }
    if (!RHI::valid(m_genPipe) || !RHI::valid(m_drawPipe) || !RHI::valid(m_pressPipe)) {
        HARUKA_LOGW("Hierba", "pipelines: gen %s · draw %s · presion %s — la hierba no se dibuja",
                    RHI::valid(m_genPipe) ? "ok" : "FALLO", RHI::valid(m_drawPipe) ? "ok" : "FALLO",
                    RHI::valid(m_pressPipe) ? "ok" : "FALLO");
        return false;
    }

    GenUBO g{}; DrawUBO d{}; PressUBO p{};
    m_genUBO   = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(g), &g, RHI::BufferMemory::Dynamic);
    m_drawUBO  = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(d), &d, RHI::BufferMemory::Dynamic);
    m_pressUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(p), &p, RHI::BufferMemory::Dynamic);
    m_nodesCap  = kNodesMax;
    m_nodesSSBO = dev->createBuffer(RHI::BufferUsage::Storage, m_nodesCap * sizeof(Haruka::Terrain::TerrainNodeRenderer::NearNode),
                                    nullptr, RHI::BufferMemory::Dynamic);
    // La lista de celdas del disco (ivec2, coords de celda de la cara): la rellena la CPU en
    // `dispatchBlades` con las celdas cuyo centro cae dentro del radio; el shader lee su celda de
    // aqui. Tope acotado: celda minima plausible 0,2 m (planeta R≥5e5) y el radio del ajuste.
    {
        const int  sideCap = 2 * ((int)std::ceil(m_cfg.radiusM / 0.2) + 1) + 1;
        m_cellCap           = (size_t)sideCap * (size_t)sideCap;
        m_cellPairs = dev->createBuffer(RHI::BufferUsage::Storage, m_cellCap * sizeof(glm::ivec2),
                                        nullptr, RHI::BufferMemory::Dynamic);
    }
    // El índice espacial de `drawnHeightAt` (binding 6): offsets por celda (kIdxDim²+1) y lista de
    // índices de nodo (kIdxDim² × kNodesMax, el peor caso: N nodos solapando todas las celdas).
    {
        const size_t nCells = (size_t)kIdxDim * (size_t)kIdxDim;
        std::vector<uint32_t> zOff(nCells + 1, 0u);
        m_idxOff  = dev->createBuffer(RHI::BufferUsage::Storage, (nCells + 1) * sizeof(uint32_t),
                                      zOff.data(), RHI::BufferMemory::Dynamic);
        m_idxList = dev->createBuffer(RHI::BufferUsage::Storage, nCells * kNodesMax * sizeof(uint32_t),
                                      nullptr, RHI::BufferMemory::Static);
    }
    m_bladesSSBO = dev->createBuffer(RHI::BufferUsage::Storage, m_cfg.maxBlades * 32, nullptr, RHI::BufferMemory::Static);
    // DrawElementsIndirectCommand: {indexCount, instanceCount, firstIndex, baseVertex, baseInstance}.
    const uint32_t cmdInit[5] = { 45u, 0u, 0u, 0u, 0u };
    m_cmd = dev->createBuffer(RHI::BufferUsage::Indirect, sizeof(cmdInit), cmdInit, RHI::BufferMemory::Dynamic);
    // La mata: 3 briznas de 7 vertices (3 pares + punta, 5 triangulos cada una). El vertex los
    // coloca por indice; la brizna `k` de la mata son los vertices 7k..7k+6.
    uint32_t idx[45];
    { const uint32_t one[15] = { 0,1,2, 2,1,3, 2,3,4, 4,3,5, 4,5,6 };
      for (uint32_t k = 0; k < 3; ++k) for (uint32_t i = 0; i < 15; ++i) idx[k * 15 + i] = one[i] + 7u * k; }
    m_ib = dev->createBuffer(RHI::BufferUsage::Index, sizeof(idx), idx, RHI::BufferMemory::Static);
    m_readback = dev->createBuffer(RHI::BufferUsage::Storage, 4 * sizeof(uint32_t), nullptr, RHI::BufferMemory::Readback);

    for (int i = 0; i < 2; ++i) {
        RHI::RenderTargetDesc rd;
        rd.width = rd.height = (uint32_t)m_cfg.pressRes;
        rd.colorFormats = { RHI::Format::RG16F };
        rd.colorFilter  = RHI::Filter::Linear;
        rd.hasDepth     = false;
        m_pressRT[i]  = dev->createRenderTarget(rd);
        m_pressTex[i] = dev->getColorTexture(m_pressRT[i], 0);
    }
    m_ready = RHI::valid(m_genUBO) && RHI::valid(m_drawUBO) && RHI::valid(m_pressUBO) && RHI::valid(m_nodesSSBO)
           && RHI::valid(m_bladesSSBO) && RHI::valid(m_cmd) && RHI::valid(m_ib)
           && RHI::valid(m_cellPairs) && RHI::valid(m_idxOff) && RHI::valid(m_idxList)
           && RHI::valid(m_pressRT[0]) && RHI::valid(m_pressRT[1]);
    if (m_ready)
        HARUKA_LOGI("Hierba", "radio %.0f m · %.0f briznas/m² · tope %zu (%.0f MB) · presion %d² a %.2f m/texel (%.0f m)",
                    m_cfg.radiusM, m_cfg.bladesPerM2 * m_cfg.density, m_cfg.maxBlades,
                    m_cfg.maxBlades * 32.0 / 1048576.0, m_cfg.pressRes, m_cfg.pressTexelM,
                    m_cfg.pressRes * m_cfg.pressTexelM);
    return m_ready;
}

void GrassRenderer::shutdown() {
    if (!m_dev) return;
    for (RHI::BufferHandle* b : { &m_genUBO, &m_drawUBO, &m_pressUBO, &m_nodesSSBO, &m_bladesSSBO, &m_cmd, &m_ib, &m_readback, &m_cellPairs, &m_idxOff, &m_idxList })
        if (RHI::valid(*b)) { m_dev->destroy(*b); *b = {}; }
    for (int i = 0; i < 2; ++i) if (RHI::valid(m_pressRT[i])) { m_dev->destroy(m_pressRT[i]); m_pressRT[i] = {}; m_pressTex[i] = {}; }
    for (RHI::PipelineHandle* p : { &m_genPipe, &m_drawPipe, &m_pressPipe })
        if (RHI::valid(*p)) { m_dev->destroy(*p); *p = {}; }
    m_ready = false; m_dev = nullptr;
}

RHI::TextureHandle GrassRenderer::pressureTexture() const { return m_pressTex[m_pressCur]; }

size_t GrassRenderer::readBack(size_t n, std::vector<float>& bladesOut) {
    bladesOut.clear();
    if (!m_ready) return 0;
    // ⚠️ LA COPIA VA EN LA COLA Y HAY QUE ESPERARLA. En Vulkan `copyBuffer` es un envio aparte que
    // espera; en OpenGL es un comando mas del stream y leer el mapa sin fence da lo del frame de
    // antes (o ceros). Mismo patron que las sondas del banco: frame con copia + barrera + fence, y
    // otro frame que la espera.
    auto copyAndWait = [&](RHI::BufferHandle src, RHI::BufferHandle dst, size_t bytes) {
        RHI::FenceHandle fence{};
        if (RHI::Context* c = m_dev->beginFrame()) {
            m_dev->copyBuffer(src, dst, 0, 0, bytes);
            c->memoryBarrier();
            fence = c->signalFence();
            m_dev->endFrame();
        }
        if (RHI::Context* c2 = m_dev->beginFrame()) {
            if (RHI::valid(fence)) { c2->waitFence(fence, 5000000000ull); c2->deleteFence(fence); }
            m_dev->endFrame();
        }
    };
    copyAndWait(m_cmd, m_readback, 4 * sizeof(uint32_t));
    const uint32_t* rb = (const uint32_t*)m_dev->mappedData(m_readback);
    const size_t count = rb ? rb[1] : 0;
    const size_t take = std::min(n, std::min(count, m_cfg.maxBlades));
    if (take > 0) {
        RHI::BufferHandle tmp = m_dev->createBuffer(RHI::BufferUsage::Storage, take * 32, nullptr, RHI::BufferMemory::Readback);
        if (RHI::valid(tmp)) {
            copyAndWait(m_bladesSSBO, tmp, take * 32);
            if (const float* p = (const float*)m_dev->mappedData(tmp)) bladesOut.assign(p, p + take * 8);
            m_dev->destroy(tmp);
        }
    }
    return count;
}

void GrassRenderer::addStamp(const glm::dvec3& worldPos, float radiusM, const glm::vec3& velocity, float strength) {
    if (m_stamps.size() >= (size_t)kMaxStamps) return;   // mas de 64 por frame: los demas no dejan huella
    m_stamps.push_back({ worldPos, radiusM, velocity, std::clamp(strength, 0.0f, 1.0f) });
}

// ── EL MAPA DE PRESION ──────────────────────────────────────────────────────────────────────────
void GrassRenderer::updatePressure(RHI::Context* ctx, const Frame& f) {
    const double sideM  = (double)m_cfg.pressRes * (double)m_cfg.pressTexelM;
    const glm::dvec3 up = glm::normalize(f.camPos - f.planetCenter);
    // El marco E/N se fija al anclar y solo se rehace si la camara se ha ido lejos (el `up` gira
    // 1e-6 rad por cada 8 m en la Tierra: en 200 m aun no se nota, y al rehacerlo el mapa se borra).
    glm::ivec2 shift(0, 0);
    if (!m_pressAnchored || glm::dot(up, m_pressUp) < std::cos(0.02)) {
        m_pressUp = up;
        m_pressE  = glm::normalize(glm::cross(up, std::abs(up.y) < 0.99 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0)));
        m_pressN  = glm::cross(up, m_pressE);
        m_pressAnchor = f.camPos;
        shift = glm::ivec2(1 << 20, 1 << 20);   // fuera del mapa viejo: todo a cero
        m_pressAnchored = true;
    } else {
        // Recentrar a saltos de texel ENTERO cuando la camara se aleja mas de un cuarto del mapa.
        const glm::dvec3 d = f.camPos - m_pressAnchor;
        const double ex = glm::dot(d, m_pressE), ny = glm::dot(d, m_pressN);
        if (std::abs(ex) > sideM * 0.25 || std::abs(ny) > sideM * 0.25) {
            shift.x = (int)std::llround(ex / m_cfg.pressTexelM);
            shift.y = (int)std::llround(ny / m_cfg.pressTexelM);
            m_pressAnchor += m_pressE * ((double)shift.x * m_cfg.pressTexelM) + m_pressN * ((double)shift.y * m_cfg.pressTexelM);
        }
    }

    PressUBO p{};
    p.shift = glm::vec4((float)shift.x, (float)shift.y, m_cfg.pressTexelM, (float)m_cfg.pressRes);
    p.decay = glm::vec4(m_cfg.recoverS, f.dt, 0.0f, 0.0f);
    int n = 0;
    for (const Stamp& s : m_stamps) {
        if (n >= kMaxStamps) break;
        const glm::dvec3 d = s.pos - m_pressAnchor;
        const glm::vec2 xy((float)glm::dot(d, m_pressE), (float)glm::dot(d, m_pressN));
        if (std::abs(xy.x) > sideM * 0.5 + s.radius || std::abs(xy.y) > sideM * 0.5 + s.radius) continue;
        glm::vec2 dir((float)glm::dot(glm::dvec3(s.vel), m_pressE), (float)glm::dot(glm::dvec3(s.vel), m_pressN));
        const float dl = glm::length(dir);
        dir = (dl > 1e-3f) ? dir / dl : glm::vec2(0.0f);
        p.stamp[n]    = glm::vec4(xy, s.radius, s.strength);
        p.stampDir[n] = glm::vec4(dir, 0.0f, 1.0f);
        ++n;
    }
    m_stamps.clear();
    m_dev->updateBuffer(m_pressUBO, 0, sizeof(p), &p);

    const int prev = m_pressCur, cur = 1 - m_pressCur;
    RHI::ClearValues cv; cv.clearColor = true; cv.clearDepth = false;
    cv.color[0] = cv.color[1] = cv.color[2] = cv.color[3] = 0.0f;
    ctx->beginRenderPass(m_pressRT[cur], cv);
    ctx->bindPipeline(m_pressPipe);
    ctx->bindUniformBuffer(0, m_pressUBO);
    ctx->bindTexture(5, m_pressTex[prev]);
    ctx->draw(3, 0, 1);
    ctx->endRenderPass();
    m_pressCur = cur;
}

// ── LA GENERACION ───────────────────────────────────────────────────────────────────────────────
void GrassRenderer::dispatchBlades(RHI::Context* ctx, const Frame& f) {
    // Lectura del contador del frame ANTERIOR (1 de cada 60): antes de resetear el comando.
    if ((m_frame % 60u) == 1u && RHI::valid(m_readback)) {
        m_dev->copyBuffer(m_cmd, m_readback, 0, 0, 4 * sizeof(uint32_t));
        if (const uint32_t* rb = (const uint32_t*)m_dev->mappedData(m_readback)) m_lastCount = rb[1];
    }
    const uint32_t cmdReset[5] = { 45u, 0u, 0u, 0u, 0u };
    m_dev->updateBuffer(m_cmd, 0, sizeof(cmdReset), cmdReset);

    // La tabla de nodos hoja de este frame.
    const auto* nodes = f.nodes;
    const size_t nNodes = nodes ? std::min(nodes->size(), m_nodesCap) : 0;
    if (nNodes == 0) return;
    m_dev->updateBuffer(m_nodesSSBO, 0, nNodes * sizeof(Haruka::Terrain::TerrainNodeRenderer::NearNode), nodes->data());

    // La celda del cubo bajo la camara y el rango que cubre el radio.
    const glm::dvec3 up = glm::normalize(f.camPos - f.planetCenter);
    Haruka::PlanetFace face; double lx, ly;
    Haruka::dirToCubeFaceClosed(up, face, lx, ly);
    const double cellM  = f.planetRadiusM * 1.5707963267948966 / (double)kCellsFace;
    const int    halfC  = (int)std::ceil((double)m_cfg.radiusM / cellM) + 1;
    const int    cx     = (int)std::floor((lx + 1.0) * 0.5 * kCellsFace);
    const int    cy     = (int)std::floor((ly + 1.0) * 0.5 * kCellsFace);
    const double cellArea = cellM * cellM;
    const uint32_t perCell = (uint32_t)std::max(1.0, std::round(cellArea * (double)m_cfg.bladesPerM2));

    // ── LAS CELDAS DENTRO DEL DISCO, NO LA REJILLA CUADRADA ───────────────────────────────────
    // El cuadrado side×side (≈55 celdas de 2,4 m en la Tierra) paga las esquinas que envuelven al
    // disco de `radiusM`. La CPU deja solo las celdas cuyo CENTRO cae dentro del radio + media
    // diagonal + 2 m: las que pierde son exactamente las que el shader ya descartaba por distancia
    // (SKIP 3), así que el resultado visual es idéntico con menos hilos que se pagan su fp64.
    // `Haruka::cubeFaceToDir` en double, la misma convención que el compute.
    const double cellD  = 2.0 / (double)kCellsFace;
    const glm::dvec3 relCam = f.camPos - f.planetCenter;
    const double altCam  = glm::length(relCam) - f.planetRadiusM;
    const double padM    = cellM * 0.71 + 2.0;
    std::vector<glm::ivec2> cells;
    cells.reserve((size_t)(2 * halfC + 1) * (2 * halfC + 1));
    for (int dy = -halfC; dy <= halfC && cells.size() < m_cellCap; ++dy) {
        for (int dx = -halfC; dx <= halfC && cells.size() < m_cellCap; ++dx) {
            const int ccx = cx + dx, ccy = cy + dy;
            if (ccx < 0 || ccy < 0 || ccx >= kCellsFace || ccy >= kCellsFace) continue;
            const double lxe = -1.0 + ((double)ccx + 0.5) * cellD;
            const double lye = -1.0 + ((double)ccy + 0.5) * cellD;
            const glm::dvec3 approx = Haruka::cubeFaceToDir(face, lxe, lye) * (f.planetRadiusM + altCam) - relCam;
            if (glm::length(approx) > (double)m_cfg.radiusM + padM) continue;
            cells.emplace_back(ccx, ccy);
        }
    }
    const uint32_t nCells = (uint32_t)cells.size();
    if (nCells == 0) return;   // sin celdas no hay briznas; el draw se salta por m_lastThreads == 0
    m_dev->updateBuffer(m_cellPairs, 0, cells.size() * sizeof(glm::ivec2), cells.data());

    // ── INDICE ESPACIAL DE LA TABLA DE NODOS ────────────────────────────────────────────────────
    // `drawnHeightAt` barría TODA la tabla por brizna (O(briznas × nodos)). Aqui se reparte cada
    // nodo por las celdas de la rejilla que su rectangulo toca (en orden de la tabla), y el shader
    // solo prueba los candidatos de su celda. La rejilla cubre el disco en coords (lx,ly) con
    // margen; fuera de ella el shader sigue barriendo la tabla entera: mismo resultado, el indice
    // solo cambia CUANTOS nodos se prueban, no cual se elige.
    const double spanL  = 2.0 * (m_cfg.radiusM + padM + cellM * 2.0) / f.planetRadiusM;
    const double gridX0 = lx - spanL * 0.5, gridY0 = ly - spanL * 0.5;
    const double cellL  = spanL / (double)kIdxDim;
    const size_t kIdxCells = (size_t)kIdxDim * (size_t)kIdxDim;
    std::vector<uint32_t> idxCnt(kIdxCells, 0u);
    std::vector<uint32_t> idxList(kIdxCells * (size_t)kNodesMax);
    for (size_t ni = 0; ni < nNodes; ++ni) {
        const auto& nd = nodes->at(ni);
        const double cellsL = std::ldexp(1.0, nd.node[1]);   // celdas por lado en el nivel del nodo
        const double cw = 2.0 / cellsL;
        const double nXA = -1.0 + (double)nd.node[2] * cw, nX1 = nXA + cw;
        const double nYA = -1.0 + (double)nd.node[3] * cw, nY1 = nYA + cw;
        if (nX1 <= gridX0 || nXA >= gridX0 + spanL || nY1 <= gridY0 || nYA >= gridY0 + spanL) continue;
        const int gx0 = (int)std::floor((nXA - gridX0) / cellL), gx1 = (int)std::floor((nX1 - gridX0) / cellL);
        const int gy0 = (int)std::floor((nYA - gridY0) / cellL), gy1 = (int)std::floor((nY1 - gridY0) / cellL);
        for (int gy = std::max(gy0, 0); gy <= std::min(gy1, kIdxDim - 1); ++gy)
            for (int gx = std::max(gx0, 0); gx <= std::min(gx1, kIdxDim - 1); ++gx) {
                const size_t c = (size_t)gy * kIdxDim + (size_t)gx;
                idxList[c * (size_t)kNodesMax + idxCnt[c]++] = (uint32_t)ni;
            }
    }
    std::vector<uint32_t> idxOff(kIdxCells + 1, 0u);
    for (size_t c = 0; c < kIdxCells; ++c) idxOff[c + 1] = idxOff[c] + idxCnt[c];
    m_dev->updateBuffer(m_idxOff, 0, idxOff.size() * sizeof(uint32_t), idxOff.data());
    if (idxOff[kIdxCells] > 0)
        m_dev->updateBuffer(m_idxList, 0, idxOff[kIdxCells] * sizeof(uint32_t), idxList.data());

    // El centro del planeta relativo a la camara, partido en grueso (multiplo de 64 m, exacto en
    // float) y fino: la misma convencion que `terrain_node.vert`. Sin esto la raiz temblaria 0,5 m.
    const glm::dvec3 rel = f.planetCenter - f.camPos;
    const double kQ = 64.0;
    const glm::dvec3 hi(std::round(rel.x / kQ) * kQ, std::round(rel.y / kQ) * kQ, std::round(rel.z / kQ) * kQ);

    GenUBO g{};
    g.center   = glm::vec4(glm::vec3(hi), (float)f.planetRadiusM);
    g.centerLo = glm::vec4(glm::vec3(rel - hi), f.seaLevelM);
    g.camFwd   = glm::vec4(glm::normalize(f.viewDir), std::cos(std::min(f.coneHalfAngle + 0.15f, 3.1f)));
    g.cells    = glm::ivec4((int)face, (int)nCells, 0, 0);
    g.grid     = glm::vec4((float)kCellsFace, (float)perCell, m_cfg.radiusM, m_cfg.density);
    g.lod      = glm::vec4(m_cfg.lodStartM, m_cfg.lodKeep, m_cfg.heightM, m_cfg.widthM);
    g.nodeGrid = glm::ivec4((int)Haruka::Terrain::TERRAIN_NODE_TEXELS, (int)Haruka::Terrain::TERRAIN_NODE_CELLS, (int)nNodes, 0);
    static const float s_dbg = [] { const char* e = std::getenv("HARUKA_GRASS_DEBUG"); return e ? (float)std::atoi(e) : 0.0f; }();
    g.misc     = glm::vec4(f.finestTexelM, s_dbg, (float)(glm::length(f.camPos - f.planetCenter) - f.planetRadiusM), 0.0f);
    g.uIdx     = glm::dvec4(gridX0, gridY0, cellL, spanL);   // en double: las fronteras de la rejilla
    g.uIdxGrid = glm::ivec4(kIdxDim, kIdxDim, 0, 0);        // del CPU y del compute coinciden al bit
    // ── LOS CUATRO PLANOS DEL FRUSTUM ─────────────────────────────────────────────────────────────
    // El cono que los envuelve (`nodeFrustumConeHalfAngle`) sobra por las esquinas: a 60° y 16:9 el
    // cono mide 49,7° de semiangulo y el rectangulo 30° x 46°, o sea que el cono genera ~1,7x de
    // matas que no estan en la imagen. Con los planos, la misma cuenta cabe DENTRO de la vista.
    {
        const glm::vec3 fwd = glm::normalize(f.viewDir);
        glm::vec3 upv = f.viewUp - fwd * glm::dot(f.viewUp, fwd);
        upv = (glm::dot(upv, upv) > 1e-6f) ? glm::normalize(upv) : glm::vec3(0, 1, 0);
        const glm::vec3 right = glm::normalize(glm::cross(fwd, upv));
        const float tv = std::max(f.tanHalfV, 0.01f), th = tv * std::max(f.aspect, 0.01f);
        // Normales hacia dentro: plano izquierdo = fwd*th + right, etc. (sin normalizar: solo el signo).
        g.plane[0] = glm::vec4(glm::normalize(fwd * th + right), 0.0f);
        g.plane[1] = glm::vec4(glm::normalize(fwd * th - right), 0.0f);
        g.plane[2] = glm::vec4(glm::normalize(fwd * tv + upv), 0.0f);
        g.plane[3] = glm::vec4(glm::normalize(fwd * tv - upv), 0.0f);
    }
    m_dev->updateBuffer(m_genUBO, 0, sizeof(g), &g);

    const uint64_t threads = (uint64_t)nCells * (uint64_t)perCell;
    m_lastThreads = (size_t)threads;
    ctx->bindPipeline(m_genPipe);
    ctx->bindUniformBuffer(0, m_genUBO);
    ctx->bindStorageBuffer(1, m_nodesSSBO);
    ctx->bindStorageBuffer(2, f.heights);
    ctx->bindStorageBuffer(3, m_bladesSSBO);
    ctx->bindStorageBuffer(4, m_cmd);
    ctx->bindStorageBuffer(5, m_cellPairs);
    ctx->bindStorageBuffer(6, m_idxOff);
    ctx->bindStorageBuffer(7, m_idxList);
    if (RHI::valid(f.materialUBO)) {
        ctx->bindUniformBuffer(12, f.materialUBO);
    } else if (!s_warnMatUBO) {
        // Si la tabla de materiales (binding 12) no llega aqui, `harukaGrassAmount` lee uMatCount=0
        // y mata TODAS las briznas (SKIP 8, grass<0.02) — un campo vacio silencioso. Se avisa UNA vez.
        s_warnMatUBO = true;
        HARUKA_LOGW("Hierba", "materialUBO (binding 12) NO disponible en el dispatch: la hierba saldra vacia (harukaGrassAmount=0)");
    }
    ctx->bindTexture(15, f.baseField);
    ctx->dispatch((uint32_t)std::min<uint64_t>((threads + 63u) / 64u, 65535u), 1, 1);
    ctx->memoryBarrier();   // el draw lee briznas y comando

    // Registro del estado que PRODUJO este dispatch: el REUSE de prepare compara contra esto (no
    // contra el frame anterior). Si la camara se queda quieta, el buffer guarda briznas relativas a
    // ESTA camara y `draw` las ancla al mundo con `camShift`.
    m_lastCam      = f.camPos;
    m_lastPlanet   = f.planetCenter;
    m_lastViewDir  = f.viewDir;
    m_lastViewUp   = f.viewUp;
    m_lastTh       = f.tanHalfV;
    m_lastAspect   = f.aspect;
    m_lastNodes    = f.nodes ? (const void*)f.nodes->data() : nullptr;
    m_lastNodesN   = f.nodes ? f.nodes->size() : 0;
}

void GrassRenderer::prepare(RHI::Context* ctx, const Frame& f) {
    if (!m_ready || !ctx || !enabled()) { m_stamps.clear(); return; }
    ++m_frame;
    static const std::string s_bis = [] { const char* e = std::getenv("HARUKA_GRASS_BIS"); return std::string(e ? e : ""); }();
    if (s_bis != "press") updatePressure(ctx, f);
    m_frameData = f;
    m_frameData.nodes = nullptr;   // el puntero es del llamante y no sobrevive al frame
    if (!RHI::valid(f.heights) || s_bis == "gen") { m_lastThreads = 0; return; }
    // ── ¿Este frame cambia algo de lo que genera? Si no, se reutiliza el dispatch anterior ──────
    static const bool s_full = [] { const char* e = std::getenv("HARUKA_GRASS_FULL"); return e && std::string(e) == "1"; }();
    int  changed = (m_first || s_full || !m_stamps.empty()) ? 1 : 0;
    if (!changed && f.nodes) {
        // m_last* es el estado del ULTIMO DISPATCH (lo rellena el final de dispatchBlades): si la
        // comparacion fuera contra el frame anterior, caminando a < 0,25 m/frame el campo nunca se
        // regeneraria y el draw (camara actual) arrastraria las briznas detras del jugador.
        changed += (glm::length(f.camPos       - m_lastCam)    > 0.25);
        changed += (glm::length(f.planetCenter - m_lastPlanet) > 0.25);
        changed += (glm::dot(glm::normalize(f.viewDir), glm::normalize(m_lastViewDir)) < 0.9995f);   // ~1,8°
        changed += (glm::dot(glm::normalize(f.viewUp),  glm::normalize(m_lastViewUp))  < 0.9995f);
        changed += (std::abs(f.tanHalfV - m_lastTh)   > 0.03f * std::max(m_lastTh, 0.01f));
        changed += (std::abs(f.aspect   - m_lastAspect) > 0.03f);
        changed += (f.nodes->data() != m_lastNodes || f.nodes->size() != m_lastNodesN);
    }
    m_first = false;
    if (!changed) return;   // conserva el contador y el buffer del frame anterior: el draw reusa
    m_lastThreads = 0;      // solo aqui: si hay 0 nodos, el draw no debe dibujar nada
    dispatchBlades(ctx, f);
}

void GrassRenderer::draw(RHI::Context* ctx, const glm::mat4& rotVP) {
    if (!m_ready || !ctx || !enabled() || m_lastThreads == 0) return;
    static const bool s_noDraw = [] { const char* e = std::getenv("HARUKA_GRASS_BIS"); return e && std::string(e) == "draw"; }();
    if (s_noDraw) return;
    // UBO del draw: la vista de ESTE pase, el viento, la luz y el marco del mapa de presion. Se
    // escribe una vez por frame y lo lee un solo draw, asi que vale aunque el pase este abierto.
    const Frame& f = m_frameData;
    DrawUBO d{};
    d.mvp    = rotVP;
    d.wind   = glm::vec4(f.wind, f.time);
    d.sun    = glm::vec4(glm::normalize(f.sunDir), f.sunIntensity);
    d.pressA = glm::vec4(glm::vec3(m_pressAnchor - m_lastCam), (float)((double)m_cfg.pressRes * m_cfg.pressTexelM));
    d.pressE = glm::vec4(glm::vec3(m_pressE), 0.0f);
    d.pressN = glm::vec4(glm::vec3(m_pressN), 0.0f);
    d.shade  = glm::vec4(f.ambient, m_cfg.widthM, m_cfg.heightM, 0.0f);
    d.aerial = f.aerial;
    d.up     = glm::vec4(glm::vec3(glm::normalize(f.camPos - f.planetCenter)), 0.0f);
    // Las briznas del buffer estan en el marco del ULTIMO dispatch (relativas a m_lastCam). El draw
    // usa la camara actual: sin compensar, el campo se translada con el jugador entre dispatches y
    // «salta» al regenerar. `camShift` lo traslada de vuelta y el ancla del mapa de presion se da en
    // el mismo marco que las briznas para que el muestreo del texel siga clavado al mundo.
    d.camShift = glm::vec4(glm::vec3(m_lastCam - f.camPos), 0.0f);
    m_dev->updateBuffer(m_drawUBO, 0, sizeof(d), &d);
    ctx->bindPipeline(m_drawPipe);
    ctx->bindUniformBuffer(0, m_drawUBO);
    ctx->bindStorageBuffer(3, m_bladesSSBO);
    ctx->bindTexture(5, m_pressTex[m_pressCur]);
    ctx->bindIndexBuffer(m_ib);
    ctx->drawIndexedIndirect(m_cmd, 1, 0, 0);
}

} // namespace Haruka::Renderer
