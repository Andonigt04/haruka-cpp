#include "Haruka/World/Props/GroundScatter.h"

#include "Haruka/Core/Logger.h"
#include "Haruka/Renderer/Shader.h"
#include "Haruka/RHI/RHIContext.h"
#include "Haruka/RHI/RHIDevice.h"
#include "Haruka/RHI/RHIGPUScope.h"
#include "Haruka/Tools/Profiler.h"
#include "Haruka/World/Terrain/CubeSphere.h"
#include "Haruka/World/Terrain/TerrainNode.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstddef>

namespace Haruka::World::Props {

using RHI::valid;

namespace {
constexpr int kMaxCraters = 32;
constexpr int kMaxZones   = 32;
constexpr int kIdxDim     = 32;   ///< lado de la rejilla del índice de nodos

/// Gemelo std140 del UBO `GroundScatter` de ground_scatter.comp. El `dvec4` va alineado a 32 B.
struct ScatterUBO {
    glm::vec4  center, centerLo, px, elem, misc, weather;
    glm::ivec4 nodeGrid, idxGrid;
    glm::dvec4 idx;
    glm::vec4  plane[4];
    glm::mat4  cutSpace;
    glm::vec4  cutInfo;
    glm::mat4  cutFineSpace;
    glm::vec4  cutAtlas;
    glm::vec4  fieldO, fieldE, fieldN;
    glm::vec4  craterC[kMaxCraters];
    glm::vec4  craterU[kMaxCraters];
    glm::vec4  zone[kMaxZones];
    glm::vec4  pad;   ///< el bloque std140 con un dvec4 redondea su tamaño a 32 B
};
static_assert(offsetof(ScatterUBO, idx) == 128, "uIdx (dvec4) tiene que caer en 128 (std140)");
static_assert(offsetof(ScatterUBO, plane) == 160, "ScatterUBO descuadrado");
static_assert(offsetof(ScatterUBO, fieldO) == 384, "ScatterUBO descuadrado");
static_assert(offsetof(ScatterUBO, craterC) == 432, "ScatterUBO descuadrado");
static_assert(sizeof(ScatterUBO) % 32 == 0, "ScatterUBO: múltiplo de 32 B");

constexpr uint32_t kCmdWords  = 8;    ///< 5 del comando + pedidas + relleno
constexpr uint32_t kInstWords = 10;   ///< 40 B por instancia (lib/instance_pack.glsl)

/// Horizontal (sobre el plano tangente de `up`) de `d`.
inline double horizLen(const glm::dvec3& d, const glm::dvec3& up) {
    return glm::length(d - up * glm::dot(d, up));
}
} // namespace

// ── el planificador (CPU, sin GPU) ────────────────────────────────────────────────────────────

int groundScatterLevel(double planetR, double cellM) {
    // La arista de una cara mide ~R·π/2; 2^level celdas por lado.
    const double want = std::max(planetR * 1.5707963267948966 / std::max(cellM, 1e-3), 1.0);
    return std::clamp((int)std::lround(std::log2(want)), 1, 28);
}

GroundScatterPlan planGroundScatter(const GroundScatterPlanInput& in) {
    GroundScatterPlan plan;
    plan.reachM = pxReachM(in.px);
    if (in.perGroup == 0 || plan.reachM <= 0.0) return plan;

    const glm::dvec3 rel = in.camPos - in.planetC;
    const double camR = glm::length(rel);
    if (camR <= 1e-6) return plan;
    const glm::dvec3 up = rel / camR;
    Tools::PlanetFace face; double lx, ly;
    Terrain::dirToCubeFaceClosed(up, face, lx, ly);
    const int    cellsFace = 1 << in.level;
    const double cellD = 2.0 / (double)cellsFace;
    const int    c0x = std::clamp((int)std::floor((lx + 1.0) * 0.5 * cellsFace), 0, cellsFace - 1);
    const int    c0y = std::clamp((int)std::floor((ly + 1.0) * 0.5 * cellsFace), 0, cellsFace - 1);

    // Plano tangente LINEALIZADO en la celda de la cámara: a 300 m la esfera se separa del plano
    // d²/2R ≈ 7 mm y la retícula de la cara es aún más recta. Así cada grupo cuesta cuatro sumas.
    const double Rg = in.planetR + in.groundM;
    auto cellPos = [&](Tools::PlanetFace fc, double cx, double cy) {
        return Terrain::cubeFaceToDir(fc, -1.0 + (cx + 0.5) * cellD, -1.0 + (cy + 0.5) * cellD) * Rg;
    };
    const glm::dvec3 P00 = cellPos(face, c0x, c0y);
    const glm::dvec3 Ex  = cellPos(face, c0x + 1, c0y) - P00;
    const glm::dvec3 Ey  = cellPos(face, c0x, c0y + 1) - P00;
    const double stepM   = std::max(std::min(glm::length(Ex), glm::length(Ey)), 1e-6);
    const double halfDiag = 0.5 * glm::length(Ex + Ey);
    const int    nR = (int)std::ceil((plan.reachM + halfDiag) / stepM) + 1;
    const bool   frustum = glm::dot(in.planes[0], in.planes[0]) > 0.0f;
    // Holgura vertical del recorte por planos: la cota real del grupo no se conoce aquí.
    const double camAlt = camR - in.planetR;
    const double vSlack = std::abs(camAlt - in.groundM) + 3.0;

    struct Cand { uint32_t face, cx, cy; double dNear; };
    std::vector<Cand> cands;
    std::vector<Cand> cross;   // los que caen en otra cara (se deduplican al final)
    auto consider = [&](const glm::dvec3& Prel, bool inFace, int cx, int cy) {
        const glm::dvec3 d = Prel - rel;
        const double dh = horizLen(d, up);
        const double dNear = std::max(0.0, dh - halfDiag);
        if (dNear >= plan.reachM) return;
        if (frustum) {
            const double rad = halfDiag + vSlack + 1.0;
            for (int i = 0; i < 4; ++i)
                if (glm::dot(d, glm::dvec3(in.planes[i])) < -rad) return;
        }
        if (inFace) { cands.push_back({ (uint32_t)face, (uint32_t)cx, (uint32_t)cy, dNear }); return; }
        // Fuera de la cara: la celda de la OTRA cara que contiene este punto.
        Tools::PlanetFace f2; double lx2, ly2;
        Terrain::dirToCubeFaceClosed(glm::normalize(Prel), f2, lx2, ly2);
        const int cx2 = std::clamp((int)std::floor((lx2 + 1.0) * 0.5 * cellsFace), 0, cellsFace - 1);
        const int cy2 = std::clamp((int)std::floor((ly2 + 1.0) * 0.5 * cellsFace), 0, cellsFace - 1);
        cross.push_back({ (uint32_t)f2, (uint32_t)cx2, (uint32_t)cy2, dNear });
    };
    for (int dy = -nR; dy <= nR; ++dy) {
        for (int dx = -nR; dx <= nR; ++dx) {
            const int cx = c0x + dx, cy = c0y + dy;
            const bool inFace = cx >= 0 && cy >= 0 && cx < cellsFace && cy < cellsFace;
            if (inFace) { consider(P00 + Ex * (double)dx + Ey * (double)dy, true, cx, cy); continue; }
            // Otra cara: su retícula no casa con esta, así que se muestrea a medio paso (sin huecos)
            // y se deduplica.
            for (int s = 0; s < 4; ++s) {
                const double ox = (s & 1) ? 0.25 : -0.25, oy = (s & 2) ? 0.25 : -0.25;
                consider(P00 + Ex * (dx + ox) + Ey * (dy + oy), false, cx, cy);
            }
        }
    }
    if (!cross.empty()) {
        std::sort(cross.begin(), cross.end(), [](const Cand& a, const Cand& b) {
            if (a.face != b.face) return a.face < b.face;
            if (a.cx != b.cx) return a.cx < b.cx;
            if (a.cy != b.cy) return a.cy < b.cy;
            return a.dNear < b.dNear;
        });
        for (size_t i = 0; i < cross.size(); ++i)
            if (i == 0 || cross[i].face != cross[i - 1].face || cross[i].cx != cross[i - 1].cx || cross[i].cy != cross[i - 1].cy)
                cands.push_back(cross[i]);
    }
    // Lo cercano primero: si se llega al tope de tandas, lo que se pierde es lo lejano.
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.dNear < b.dNear; });

    plan.work.reserve(cands.size() * 2);
    for (const Cand& c : cands) {
        const uint32_t n = pxGroupThreads(in.px, in.perGroup, (float)c.dNear);
        if (n == 0) continue;
        const uint32_t batches = (n + 63u) / 64u;
        if (plan.work.size() + batches > in.maxWork) { plan.clipped = true; break; }
        for (uint32_t b = 0; b < batches; ++b)
            plan.work.push_back({ c.cx, c.cy, c.face | ((b * 64u) << 3), n });
        ++plan.groups;
        plan.threads += n;
    }
    return plan;
}

// ── GPU ───────────────────────────────────────────────────────────────────────────────────────

bool GroundScatter::init(RHI::Device* dev, const Config& cfg) {
    shutdown();
    if (!dev) return false;
    m_dev = dev; m_cfg = cfg;
    const std::string cs = Renderer::Shader::baseDir() + "shaders/ground_scatter.comp";
    { RHI::PipelineDesc pd; pd.computePath = cs.c_str(); m_pipe = dev->createPipeline(pd); }
    m_ubo = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(ScatterUBO), nullptr, RHI::BufferMemory::Dynamic);
    // La salida la escribe el compute y la lee el stream de vértices: Storage (+ Vertex en Vulkan).
    m_inst = dev->createBuffer(RHI::BufferUsage::Storage, (size_t)m_cfg.capacity * kInstWords * 4u, nullptr, RHI::BufferMemory::Static);
    const uint32_t cmd0[kCmdWords] = {};
    m_cmd = dev->createBuffer(RHI::BufferUsage::Indirect, sizeof(cmd0), cmd0, RHI::BufferMemory::Dynamic);
    m_readback = dev->createBuffer(RHI::BufferUsage::Storage, sizeof(cmd0), nullptr, RHI::BufferMemory::Readback);
    {
        const uint8_t black[4] = { 0, 0, 0, 0 };
        RHI::TextureDesc dd;
        dd.width = dd.height = 1; dd.format = RHI::Format::RGBA8;
        dd.filter = RHI::Filter::Nearest; dd.wrap = RHI::Wrap::ClampToEdge;
        dd.mipmaps = false; dd.initialData = black;
        m_dummy2D = dev->createTexture(dd);
    }
    m_ready = valid(m_pipe) && valid(m_ubo) && valid(m_inst) && valid(m_cmd) && valid(m_dummy2D);
    if (!m_ready) {
        HARUKA_LOGW("GroundScatter", "no se pudo crear (compute %s) — la cubierta densa no se dibuja",
                    valid(m_pipe) ? "ok" : "FALLO");
        shutdown();
        return false;
    }
    // Sin medida de GPU el lazo solo baja por el tope duro: se arranca a media escala, no al máximo.
    m_ctl = LoadController{};
    m_ctl.budgetMs = m_cfg.budgetMs;
    m_ctl.detail   = 0.5f;
    if (const char* e = std::getenv("HARUKA_COVER_BUDGET_MS")) m_ctl.budgetMs = std::max(0.1, std::atof(e));
    HARUKA_LOGI("GroundScatter", "%.0f elementos/m² · grupo %.2f m · tope %u (%.1f MB) · presupuesto %.1f ms",
                m_cfg.elemPerM2, m_cfg.cellM, m_cfg.capacity, gpuBytes() / 1048576.0, m_ctl.budgetMs);
    return true;
}

void GroundScatter::shutdown() {
    if (m_dev) {
        for (RHI::BufferHandle* b : { &m_ubo, &m_nodes, &m_work, &m_idxOff, &m_idxList, &m_inst, &m_cmd, &m_readback })
            if (valid(*b)) m_dev->destroy(*b);
        if (valid(m_dummy2D)) m_dev->destroy(m_dummy2D);
        if (valid(m_pipe)) m_dev->destroy(m_pipe);
    }
    m_ubo = m_nodes = m_work = m_idxOff = m_idxList = m_inst = m_cmd = m_readback = {};
    m_dummy2D = {}; m_pipe = {};
    m_nodesCap = m_workCap = m_idxListCap = 0;
    m_ready = false; m_dev = nullptr; m_lastWork = 0;
}

size_t GroundScatter::gpuBytes() const {
    return (size_t)m_cfg.capacity * kInstWords * 4u + m_workCap + m_nodesCap + m_idxListCap + sizeof(ScatterUBO);
}

float GroundScatter::thrPx() const {
    static const int s_forced = [] { const char* e = std::getenv("HARUKA_COVER_DETAIL"); return e ? std::atoi(e) : -1; }();
    const float d = (s_forced >= 0) ? std::clamp((float)s_forced / 1000.0f, 0.0f, 1.0f) : m_ctl.detail;
    return pxThrPx(d, m_cfg.thrMinPx, m_cfg.thrMaxPx);
}

void GroundScatter::feedGpuMs(double ms, double now) {
    if (!m_ready) return;
    m_ctl.update(ms, now, m_over);
}

GroundViewIndex buildGroundViewIndex(const Planet::GroundView& g, const glm::dvec3& camPos,
                                     double reachM, int dim) {
    GroundViewIndex gi;
    gi.dim = dim;
    const double R = g.planetRadiusM;
    const glm::dvec3 up = glm::normalize(camPos - g.planetCenter);
    Tools::PlanetFace camFace; double lx, ly;
    Terrain::dirToCubeFaceClosed(up, camFace, lx, ly);
    gi.span = 2.0 * reachM / R;
    gi.x0 = lx - gi.span * 0.5; gi.y0 = ly - gi.span * 0.5;
    gi.cell = gi.span / (double)dim;
    std::vector<std::vector<uint32_t>> lists((size_t)dim * dim);
    for (size_t ni = 0; ni < g.nodes.size(); ++ni) {
        const auto& nd = g.nodes[ni];
        if (nd.node[0] != (int32_t)camFace) continue;   // las otras caras van por el barrido completo
        const double cw = 2.0 / std::ldexp(1.0, nd.node[1]);
        const double nX0 = -1.0 + (double)nd.node[2] * cw, nX1 = nX0 + cw;
        const double nY0 = -1.0 + (double)nd.node[3] * cw, nY1 = nY0 + cw;
        if (nX1 <= gi.x0 || nX0 >= gi.x0 + gi.span || nY1 <= gi.y0 || nY0 >= gi.y0 + gi.span) continue;
        const int gx0 = std::max((int)std::floor((nX0 - gi.x0) / gi.cell), 0);
        const int gx1 = std::min((int)std::floor((nX1 - gi.x0) / gi.cell), dim - 1);
        const int gy0 = std::max((int)std::floor((nY0 - gi.y0) / gi.cell), 0);
        const int gy1 = std::min((int)std::floor((nY1 - gi.y0) / gi.cell), dim - 1);
        for (int gy = gy0; gy <= gy1; ++gy)
            for (int gx = gx0; gx <= gx1; ++gx) lists[(size_t)gy * dim + (size_t)gx].push_back((uint32_t)ni);
    }
    gi.off.assign(lists.size() + 1, 0u);
    for (size_t c = 0; c < lists.size(); ++c) {
        gi.off[c + 1] = gi.off[c] + (uint32_t)lists[c].size();
        gi.list.insert(gi.list.end(), lists[c].begin(), lists[c].end());
    }
    if (gi.list.empty()) gi.list.push_back(0u);   // un SSBO vacío no se puede atar
    return gi;
}

bool GroundScatter::dispatch(RHI::Context* ctx, const Frame& f, uint32_t indexCount) {
    m_lastWork = 0;
    if (!m_ready || !ctx || !f.ground || f.ground->nodes.empty() || indexCount == 0) return false;
    HARUKA_PROFILE("ground.scatter"); HARUKA_GPU_SCOPE("ground.scatter");
    const Planet::GroundView& g = *f.ground;
    ++m_frame;

    // Lo que contó el compute de hace unos frames (el comando del frame anterior ya terminó: el
    // device espera en endFrame). Cada 15 frames basta para el tope duro.
    if ((m_frame % 15u) == 1u && valid(m_readback)) {
        m_dev->copyBuffer(m_cmd, m_readback, 0, 0, kCmdWords * 4u);
        if (const uint32_t* rb = (const uint32_t*)m_dev->mappedData(m_readback)) {
            m_lastCount = rb[1]; m_lastWanted = rb[5];
            m_over = m_lastWanted > m_cfg.capacity;
        }
    }

    // ── EL PLAN ──────────────────────────────────────────────────────────────────────────────
    const double R = g.planetRadiusM;
    const int level = groundScatterLevel(R, m_cfg.cellM);
    const double cellM = R * 1.5707963267948966 / (double)(1 << level);
    const uint32_t perGroup = (uint32_t)std::clamp(std::lround(m_cfg.elemPerM2 * cellM * cellM), 1L, 4096L);

    GroundScatterPlanInput pin;
    pin.camPos = f.camPos; pin.planetC = g.planetCenter; pin.planetR = R; pin.groundM = f.groundM;
    pin.px.focalPx  = (float)(0.5 * (double)f.heightPx / std::max((double)f.tanHalfV, 1e-3));
    pin.px.widthM   = m_cfg.widthM;
    pin.px.thrPx    = thrPx();
    pin.px.widenMax = m_cfg.widenMax;
    pin.perGroup = perGroup; pin.level = level;
    glm::vec4 planes[4];
    {
        const glm::vec3 fwd = glm::normalize(f.viewDir);
        glm::vec3 upv = f.viewUp - fwd * glm::dot(f.viewUp, fwd);
        upv = (glm::dot(upv, upv) > 1e-6f) ? glm::normalize(upv) : glm::vec3(0, 1, 0);
        const glm::vec3 right = glm::normalize(glm::cross(fwd, upv));
        const float tv = std::max(f.tanHalfV, 0.01f), th = tv * std::max(f.aspect, 0.01f);
        planes[0] = glm::vec4(glm::normalize(fwd * th + right), 0.0f);
        planes[1] = glm::vec4(glm::normalize(fwd * th - right), 0.0f);
        planes[2] = glm::vec4(glm::normalize(fwd * tv + upv), 0.0f);
        planes[3] = glm::vec4(glm::normalize(fwd * tv - upv), 0.0f);
        for (int i = 0; i < 4; ++i) pin.planes[i] = glm::vec3(planes[i]);
    }
    GroundScatterPlan plan;
    { HARUKA_PROFILE("ground.scatter.plan"); plan = planGroundScatter(pin); }
    m_lastReach = plan.reachM; m_lastGroups = plan.groups; m_lastThreads = plan.threads;
    if (plan.clipped) m_over = true;
    if (plan.work.empty()) return false;

    // ── LOS BUFFERS DEL FRAME ────────────────────────────────────────────────────────────────
    // Se reasignan al crecer (el frame anterior ya terminó: el device espera en endFrame).
    auto put = [&](RHI::BufferHandle& buf, size_t& cap, const void* data, size_t bytes) {
        if (cap < bytes) {
            if (valid(buf)) m_dev->destroy(buf);
            cap = bytes + bytes / 2 + 1024;
            buf = m_dev->createBuffer(RHI::BufferUsage::Storage, cap, nullptr, RHI::BufferMemory::Dynamic);
        }
        if (valid(buf) && bytes > 0) m_dev->updateBuffer(buf, 0, bytes, data);
    };
    put(m_work, m_workCap, plan.work.data(), plan.work.size() * sizeof(GroundScatterWork));
    put(m_nodes, m_nodesCap, g.nodes.data(), g.nodes.size() * sizeof(g.nodes[0]));

    // Índice espacial de la tabla de nodos sobre la cara de la cámara (ver lib/ground_view.glsl).
    const GroundViewIndex gi = buildGroundViewIndex(g, f.camPos, plan.reachM + 2.0 * cellM + 4.0, kIdxDim);
    const std::vector<uint32_t>& idxOff = gi.off;
    const std::vector<uint32_t>& idxList = gi.list;
    const double gridX0 = gi.x0, gridY0 = gi.y0, cellL = gi.cell, spanL = gi.span;
    if (!valid(m_idxOff))   // tamaño fijo: kIdxDim² + 1
        m_idxOff = m_dev->createBuffer(RHI::BufferUsage::Storage, idxOff.size() * 4u, nullptr, RHI::BufferMemory::Dynamic);
    if (valid(m_idxOff)) m_dev->updateBuffer(m_idxOff, 0, idxOff.size() * 4u, idxOff.data());
    put(m_idxList, m_idxListCap, idxList.data(), idxList.size() * 4u);
    if (!valid(m_work) || !valid(m_nodes) || !valid(m_idxOff) || !valid(m_idxList)) return false;

    // ── EL UBO ───────────────────────────────────────────────────────────────────────────────
    ScatterUBO u{};
    // Centro del planeta relativo a la cámara, en grueso (múltiplo de 64 m, exacto en float) + fino:
    // la convención de terrain_node.vert.
    const glm::dvec3 relC = g.planetCenter - f.camPos;
    const double kQ = 64.0;
    const glm::dvec3 hi(std::round(relC.x / kQ) * kQ, std::round(relC.y / kQ) * kQ, std::round(relC.z / kQ) * kQ);
    u.center   = glm::vec4(glm::vec3(hi), (float)R);
    u.centerLo = glm::vec4(glm::vec3(relC - hi), g.seaLevelM);
    u.px       = glm::vec4(pin.px.focalPx, pin.px.widthM, pin.px.thrPx, pin.px.widenMax);
    u.elem     = glm::vec4((float)perGroup, (float)(1 << level), m_cfg.scaleMin, m_cfg.scaleMax);
    float seedF; std::memcpy(&seedF, &m_cfg.seed, sizeof(seedF));
    u.misc     = glm::vec4(g.finestTexelM, seedF, m_cfg.density, m_cfg.sinkM);
    u.weather  = glm::vec4(std::clamp(f.snow, 0.0f, 1.0f), m_cfg.bendRad, m_cfg.maxSlope, 0.0f);
    const int nCr = (int)std::min<size_t>({ g.craterC.size(), g.craterU.size(), (size_t)kMaxCraters });
    const int nZn = (int)std::min<size_t>(f.zones.size(), (size_t)kMaxZones);
    u.nodeGrid = glm::ivec4((int)Terrain::TERRAIN_NODE_TEXELS, (int)Terrain::TERRAIN_NODE_CELLS, (int)g.nodes.size(), nCr);
    u.idxGrid  = glm::ivec4(kIdxDim, kIdxDim, (int)m_cfg.capacity, nZn);
    u.idx      = glm::dvec4(gridX0, gridY0, cellL, spanL);
    for (int i = 0; i < 4; ++i) u.plane[i] = planes[i];
    const bool hasCut = valid(g.cutTex);
    u.cutSpace     = g.cutSpace;
    u.cutInfo      = glm::vec4(hasCut ? 1.0f : 0.0f, 0.0f, hasCut ? g.cutFineR : 0.0f, 0.0f);
    u.cutFineSpace = g.cutFineSpace;
    u.cutAtlas     = hasCut ? g.cutAtlas : glm::vec4(0.0f);
    if (valid(f.fieldTex) && f.field.res > 0) {
        u.fieldO = glm::vec4(glm::vec3(f.field.anchor - f.camPos), f.field.texelM);
        u.fieldE = glm::vec4(glm::vec3(f.field.east), (float)f.field.res);
        u.fieldN = glm::vec4(glm::vec3(f.field.north), 0.0f);
    }
    for (int i = 0; i < nCr; ++i) { u.craterC[i] = g.craterC[(size_t)i]; u.craterU[i] = g.craterU[(size_t)i]; }
    for (int i = 0; i < nZn; ++i) u.zone[i] = f.zones[(size_t)i];
    m_dev->updateBuffer(m_ubo, 0, sizeof(u), &u);

    const uint32_t cmd0[kCmdWords] = { indexCount, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    m_dev->updateBuffer(m_cmd, 0, sizeof(cmd0), cmd0);

    ctx->bindPipeline(m_pipe);
    ctx->bindUniformBuffer(0, m_ubo);
    if (valid(g.materialUBO)) ctx->bindUniformBuffer(12, g.materialUBO);
    ctx->bindStorageBuffer(1, m_nodes);
    ctx->bindStorageBuffer(2, g.heights);
    ctx->bindStorageBuffer(3, m_inst);
    ctx->bindStorageBuffer(4, m_cmd);
    ctx->bindStorageBuffer(5, m_work);
    ctx->bindStorageBuffer(6, m_idxOff);
    ctx->bindStorageBuffer(7, m_idxList);
    ctx->bindTexture(15, g.baseField);
    ctx->bindTexture(16, hasCut ? g.cutTex : m_dummy2D);
    ctx->bindTexture(17, valid(f.fieldTex) ? f.fieldTex : m_dummy2D);
    ctx->dispatch((uint32_t)plan.work.size(), 1, 1);
    ctx->memoryBarrier();   // el dibujo lee instancias (vértices) y el comando (indirecto)
    m_lastWork = (uint32_t)plan.work.size();
    return true;
}

uint32_t GroundScatter::readBack(std::vector<uint32_t>& cmdOut, std::vector<uint32_t>& instOut, uint32_t maxInst) {
    cmdOut.clear(); instOut.clear();
    if (!m_ready) return 0;
    // La copia va en la cola y hay que esperarla (en GL es un comando más del stream; en Vulkan un
    // envío aparte): frame con copia + barrera + fence, y otro frame que la espera.
    auto copyAndWait = [&](RHI::BufferHandle src, RHI::BufferHandle dst, size_t bytes) {
        RHI::FenceHandle fence{};
        if (RHI::Context* c = m_dev->beginFrame()) {
            c->memoryBarrier();
            m_dev->copyBuffer(src, dst, 0, 0, bytes);
            c->memoryBarrier();
            fence = c->signalFence();
            m_dev->endFrame();
        }
        if (RHI::Context* c2 = m_dev->beginFrame()) {
            if (valid(fence)) { c2->waitFence(fence, 5000000000ull); c2->deleteFence(fence); }
            m_dev->endFrame();
        }
    };
    copyAndWait(m_cmd, m_readback, kCmdWords * 4u);
    if (const uint32_t* rb = (const uint32_t*)m_dev->mappedData(m_readback)) cmdOut.assign(rb, rb + kCmdWords);
    const uint32_t count = cmdOut.size() > 1 ? cmdOut[1] : 0u;
    const uint32_t take = std::min({ count, maxInst, m_cfg.capacity });
    if (take > 0) {
        RHI::BufferHandle tmp = m_dev->createBuffer(RHI::BufferUsage::Storage, (size_t)take * kInstWords * 4u, nullptr, RHI::BufferMemory::Readback);
        if (valid(tmp)) {
            copyAndWait(m_inst, tmp, (size_t)take * kInstWords * 4u);
            if (const uint32_t* p = (const uint32_t*)m_dev->mappedData(tmp)) instOut.assign(p, p + (size_t)take * kInstWords);
            m_dev->destroy(tmp);
        }
    }
    return count;
}

} // namespace Haruka::World::Props
