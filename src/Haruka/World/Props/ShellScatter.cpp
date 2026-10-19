#include "Haruka/World/Props/ShellScatter.h"

#include "Haruka/Core/Logger.h"
#include "Haruka/Renderer/FallbackTexture.h"
#include "Haruka/Renderer/GPUInstancing.h"
#include "Haruka/Renderer/Shader.h"
#include "Haruka/RHI/RHIContext.h"
#include "Haruka/RHI/RHIDevice.h"
#include "Haruka/RHI/RHIGPUScope.h"
#include "Haruka/Tools/Profiler.h"
#include "Haruka/World/Props/BlobSurface.h"
#include "Haruka/World/Props/PropGpu.h"
#include "Haruka/World/ShellClumps.h"
#include "Haruka/World/Terrain/CubeSphere.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <unordered_set>

namespace Haruka::World::Props {

using RHI::valid;

namespace {
constexpr uint32_t kBlock     = 8;                                   ///< celdas por lado de un bloque
constexpr uint32_t kCmdWords  = 5u * ShellScatter::kRegions + ShellScatter::kRegions;   ///< un comando + una pedida por región
constexpr uint32_t kInstWords = 10;                                  ///< 40 B por instancia (lib/instance_pack.glsl)
constexpr uint32_t kVolWords  = 6;                                   ///< envolvente por instancia (`uVol` de shell_scatter.comp)

/// Gemelo std140 del UBO `ShellScatter` de shell_scatter.comp.
struct ScatterUBO {
    glm::mat4  rot;
    glm::vec4  center, grid, px, lod;
    glm::vec4  plane[4];
    glm::uvec4 cap;
    glm::vec4  shape, wind;
};
static_assert(offsetof(ScatterUBO, plane) == 128, "ScatterUBO descuadrado (std140)");
static_assert(sizeof(ScatterUBO) == 240, "ScatterUBO descuadrado (std140)");
} // namespace

// ── el planificador (CPU, sin GPU) ────────────────────────────────────────────────────────────

uint32_t shellScatterCells(double R, double cellM) {
    return (uint32_t)std::clamp(std::lround(R * 1.5707963267948966 / std::max(cellM, 1.0)), 8L, 1L << 20);
}

double shellHorizonM(double planetR, double camR, double shellHiM) {
    const double rc = std::max(camR, planetR);
    const double rs = planetR + std::max(shellHiM, 0.0);
    return std::sqrt(std::max(rc * rc - planetR * planetR, 0.0)) + std::sqrt(std::max(rs * rs - planetR * planetR, 0.0));
}

ShellScatterPlan planShellScatter(const ShellScatterPlanInput& in) {
    ShellScatterPlan plan;
    const glm::dvec3 rel = in.camPos - in.planetC;
    const double camR = glm::length(rel);
    if (camR <= 1e-6 || in.reachM <= 0.0 || in.cells == 0) return plan;
    const double R = in.planetR;
    const glm::dvec3 upW = rel / camR;
    const glm::dquat inv = glm::inverse(in.rot);
    const glm::dvec3 upC = glm::normalize(inv * upW);   // la vertical de la cámara EN el marco de la retícula
    const double alpha = std::min(in.reachM / R, 3.14159265358979);
    const uint32_t N = in.cells;
    const uint32_t blocks = (N + kBlock - 1) / kBlock;
    const double cellAng = 1.5707963267948966 / (double)N;
    // Medio bloque en ángulo, con la holgura del spherify (las celdas de las esquinas de la cara miden
    // hasta ~1,25 veces las del centro).
    const double blockHalfAng = 0.5 * std::sqrt(2.0) * kBlock * cellAng * 1.3;
    const double hMid = 0.5 * (in.shellLoM + in.shellHiM);
    const double vHalf = 0.5 * std::max(in.shellHiM - in.shellLoM, 0.0);
    const bool frustum = glm::dot(in.planes[0], in.planes[0]) > 0.0f;

    // Caja de cada cara que toca el casquete: centro y anillos del borde, llevados a (cara, lx, ly).
    double lo[6][2], hi[6][2];
    bool touched[6] = {};
    auto mark = [&](const glm::dvec3& d) {
        Tools::PlanetFace f; double lx, ly;
        Terrain::dirToCubeFaceClosed(d, f, lx, ly);
        const int fi = (int)f;
        if (!touched[fi]) { lo[fi][0] = hi[fi][0] = lx; lo[fi][1] = hi[fi][1] = ly; touched[fi] = true; }
        lo[fi][0] = std::min(lo[fi][0], lx); hi[fi][0] = std::max(hi[fi][0], lx);
        lo[fi][1] = std::min(lo[fi][1], ly); hi[fi][1] = std::max(hi[fi][1], ly);
    };
    const glm::dvec3 ref = (std::abs(upC.y) < 0.99) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const glm::dvec3 t1 = glm::normalize(glm::cross(ref, upC)), t2 = glm::cross(upC, t1);
    mark(upC);
    const double outer = std::min(alpha + blockHalfAng, 3.14159265358979);
    for (int ring = 1; ring <= 6; ++ring) {
        const double a = outer * (double)ring / 6.0;
        const int nSeg = 24 * ring;
        for (int s = 0; s < nSeg; ++s) {
            const double th = 6.283185307179586 * (double)s / (double)nSeg;
            mark(upC * std::cos(a) + (t1 * std::cos(th) + t2 * std::sin(th)) * std::sin(a));
        }
    }

    struct Cand { ShellScatterBlock b; double d; };
    std::vector<Cand> cands;
    const double cosOuter = std::cos(std::min(alpha + blockHalfAng, 3.14159265358979));
    for (int fi = 0; fi < 6; ++fi) {
        if (!touched[fi]) continue;
        // De (lx, ly) a bloque, con un bloque de holgura por lado (el muestreo del borde es discreto).
        auto toBlock = [&](double l) { return (int)std::floor((l + 1.0) * 0.5 * (double)N / (double)kBlock); };
        const int bx0 = std::max(toBlock(lo[fi][0]) - 1, 0), bx1 = std::min(toBlock(hi[fi][0]) + 1, (int)blocks - 1);
        const int by0 = std::max(toBlock(lo[fi][1]) - 1, 0), by1 = std::min(toBlock(hi[fi][1]) + 1, (int)blocks - 1);
        for (int by = by0; by <= by1; ++by)
            for (int bx = bx0; bx <= bx1; ++bx) {
                const double cx = std::min((double)(bx * kBlock) + 0.5 * kBlock, (double)N);
                const double cy = std::min((double)(by * kBlock) + 0.5 * kBlock, (double)N);
                const glm::dvec3 dC = Terrain::cubeFaceToDir((Tools::PlanetFace)fi, -1.0 + 2.0 * cx / N, -1.0 + 2.0 * cy / N);
                if (glm::dot(dC, upC) < cosOuter) continue;
                const glm::dvec3 P = in.planetC + (in.rot * dC) * (R + hMid) - in.camPos;
                if (frustum) {
                    const double rad = blockHalfAng * R + vHalf + in.spillM;
                    bool out = false;
                    for (int p = 0; p < 4 && !out; ++p) out = glm::dot(P, glm::dvec3(in.planes[p])) < -rad;
                    if (out) continue;
                }
                cands.push_back({ { (uint32_t)fi, (uint32_t)bx, (uint32_t)by, 0u }, glm::length(P) });
            }
    }
    plan.candidates = (uint32_t)cands.size();
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
    if (cands.size() > in.maxWork) { plan.clipped = true; cands.resize(in.maxWork); }
    plan.work.reserve(cands.size());
    for (const Cand& c : cands) plan.work.push_back(c.b);
    return plan;
}

// ── las mallas ────────────────────────────────────────────────────────────────────────────────

bool ShellScatter::buildMeshes() {
    // LAS NUBES, HORNEADAS: cada variante es un racimo de la ley (semiancho 1, alto kClumpBakeH) con
    // sus bultos FUNDIDOS en una sola superficie (`BlobSurface`: unión suave + relieve de bolitas +
    // base plana). Celdas de la rejilla por nivel: de cerca se ve la bolita, de lejos la silueta.
    static constexpr int kCells[kLods] = { 48, 22, 10, 5 };
    for (int v = 0; v < kVariants; ++v) {
        const uint32_t h = clumpHash(0xC10D0000u + (uint32_t)v * 7919u);
        Clump c;
        c.present = true; c.widthM = 2.0f; c.heightM = kClumpBakeH; c.strength = 1.0f;
        clumpSetShape(c, h);
        std::vector<ClumpBlob> blobs;
        for (int k = 0; k < c.blobs; ++k) blobs.push_back(clumpBlob(c, h, k));
        for (int lod = 0; lod < kLods; ++lod) {
            BlobSurfaceParams prm;
            prm.seed = h; prm.cells = kCells[lod];
            std::vector<PropVertex> verts; std::vector<uint32_t> idx;
            if (!bakeBlobSurface(blobs, prm, verts, idx)) return false;
            const int r = v * kLods + lod;
            m_vbo[r] = m_dev->createBuffer(RHI::BufferUsage::Vertex, verts.size() * sizeof(PropVertex), verts.data());
            m_ebo[r] = m_dev->createBuffer(RHI::BufferUsage::Index, idx.size() * sizeof(uint32_t), idx.data());
            m_meshIdx[r] = (uint32_t)idx.size();
            if (!valid(m_vbo[r]) || !valid(m_ebo[r])) return false;
        }
    }
    HARUKA_LOGI("ShellScatter", "%d nubes horneadas · caras por nivel %u / %u / %u / %u (la primera)", kVariants,
                m_meshIdx[0] / 3u, m_meshIdx[1] / 3u, m_meshIdx[2] / 3u, m_meshIdx[3] / 3u);
    return true;
}

// ── GPU ───────────────────────────────────────────────────────────────────────────────────────

bool ShellScatter::init(RHI::Device* dev, const Config& cfg) {
    shutdown();
    if (!dev) return false;
    m_dev = dev; m_cfg = cfg;
    const std::string cs = Renderer::Shader::baseDir() + "shaders/shell_scatter.comp";
    { RHI::PipelineDesc pd; pd.computePath = cs.c_str(); m_pipe = dev->createPipeline(pd); }
    {
        const std::string vs = Renderer::Shader::baseDir() + "shaders/prop_vol.vert";
        const std::string fs = Renderer::Shader::baseDir() + "shaders/prop_vol.frag";
        RHI::PipelineDesc pd;
        pd.vertexPath = vs.c_str(); pd.fragmentPath = fs.c_str();
        appendPropVertexLayout(pd.vertexLayout, /*withPart*/false);
        Renderer::GPUInstancing::appendInstanceLayout(pd.vertexLayout, 1);
        // La envolvente: segundo stream por instancia (binding 2), en el mismo hueco que la instancia.
        pd.vertexLayout.strides.resize(3, 0);
        pd.vertexLayout.rates.resize(3, RHI::InputRate::Vertex);
        pd.vertexLayout.strides[2] = kVolWords * 4u;
        pd.vertexLayout.rates[2]   = RHI::InputRate::Instance;
        pd.vertexLayout.attributes.push_back({ 6, 0u,  RHI::Format::RGBA32F, 2 });
        pd.vertexLayout.attributes.push_back({ 8, 16u, RHI::Format::RG32F,   2 });
        pd.topology = RHI::PrimitiveTopology::Triangles;
        pd.depth.test = true; pd.depth.write = true;
        pd.blend.enable = false;
        pd.cull = RHI::CullMode::Back;
        m_drawPSO = dev->createPipeline(pd);
    }
    m_ubo = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(ScatterUBO), nullptr, RHI::BufferMemory::Dynamic);
    m_params = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PropParams), nullptr, RHI::BufferMemory::Dynamic);
    m_inst = dev->createBuffer(RHI::BufferUsage::Storage, (size_t)m_cfg.capacity * kRegions * kInstWords * 4u, nullptr, RHI::BufferMemory::Static);
    m_vol  = dev->createBuffer(RHI::BufferUsage::Storage, (size_t)m_cfg.capacity * kRegions * kVolWords * 4u, nullptr, RHI::BufferMemory::Static);
    const uint32_t cmd0[kCmdWords] = {};
    m_cmd = dev->createBuffer(RHI::BufferUsage::Indirect, sizeof(cmd0), cmd0, RHI::BufferMemory::Dynamic);
    m_readback = dev->createBuffer(RHI::BufferUsage::Storage, sizeof(cmd0), nullptr, RHI::BufferMemory::Readback);
    {
        const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        RHI::TextureDesc dd;
        dd.width = dd.height = 1; dd.format = RHI::Format::RGBA32F;
        dd.filter = RHI::Filter::Nearest; dd.wrap = RHI::Wrap::Repeat;
        dd.mipmaps = false; dd.initialData = zero;
        m_dummy = dev->createTexture(dd);
    }
    m_ready = valid(m_pipe) && valid(m_drawPSO) && valid(m_ubo) && valid(m_params) && valid(m_inst) && valid(m_vol)
           && valid(m_cmd) && valid(m_dummy) && buildMeshes();
    if (!m_ready) {
        HARUKA_LOGW("ShellScatter", "no se pudo crear (compute %s · dibujo %s) — no hay racimos",
                    valid(m_pipe) ? "ok" : "FALLO", valid(m_drawPSO) ? "ok" : "FALLO");
        shutdown();
        return false;
    }
    m_ctl = LoadController{};
    m_ctl.budgetMs = m_cfg.budgetMs;
    m_ctl.detail   = 0.6f;
    if (const char* e = std::getenv("HARUKA_CLUMP_BUDGET_MS")) m_ctl.budgetMs = std::max(0.1, std::atof(e));
    HARUKA_LOGI("ShellScatter", "celda %.0f m · tope %u por región (%.1f MB) · presupuesto %.1f ms",
                m_cfg.cellM, m_cfg.capacity, (double)m_cfg.capacity * kRegions * kInstWords * 4.0 / 1048576.0,
                m_ctl.budgetMs);
    return true;
}

void ShellScatter::shutdown() {
    if (m_dev) {
        for (RHI::BufferHandle* b : { &m_ubo, &m_params, &m_work, &m_inst, &m_vol, &m_cmd, &m_readback })
            if (valid(*b)) m_dev->destroy(*b);
        for (int r = 0; r < kRegions; ++r) {
            if (valid(m_vbo[r])) m_dev->destroy(m_vbo[r]);
            if (valid(m_ebo[r])) m_dev->destroy(m_ebo[r]);
        }
        if (valid(m_dummy)) m_dev->destroy(m_dummy);
        if (valid(m_pipe)) m_dev->destroy(m_pipe);
        if (valid(m_drawPSO)) m_dev->destroy(m_drawPSO);
    }
    m_ubo = m_params = m_work = m_inst = m_vol = m_cmd = m_readback = {};
    for (int r = 0; r < kRegions; ++r) { m_vbo[r] = {}; m_ebo[r] = {}; m_meshIdx[r] = 0; }
    m_dummy = {}; m_pipe = {}; m_drawPSO = {};
    m_workCap = 0;
    m_ready = false; m_dev = nullptr; m_lastWork = 0;
}

float ShellScatter::pxPerBlob() const {
    const float t = std::clamp(m_ctl.detail, 0.0f, 1.0f);
    return m_cfg.pxPerBlobMax * std::pow(m_cfg.pxPerBlobMin / m_cfg.pxPerBlobMax, t);
}
float ShellScatter::cullPx() const {
    const float t = std::clamp(m_ctl.detail, 0.0f, 1.0f);
    return m_cfg.cullPxMax * std::pow(m_cfg.cullPxMin / m_cfg.cullPxMax, t);
}

void ShellScatter::feedGpuMs(double ms, double now) {
    if (!m_ready) return;
    m_ctl.update(ms, now, m_over);
}

bool ShellScatter::dispatch(RHI::Context* ctx, const Frame& f) {
    m_lastWork = 0;
    if (!m_ready || !ctx) return false;
    HARUKA_PROFILE("shell.scatter"); HARUKA_GPU_SCOPE("shell.scatter");
    ++m_frame;

    // Lo que contó el compute de hace unos frames (el device espera en endFrame): el tope duro.
    if ((m_frame % 15u) == 1u && valid(m_readback)) {
        m_dev->copyBuffer(m_cmd, m_readback, 0, 0, kCmdWords * 4u);
        if (const uint32_t* rb = (const uint32_t*)m_dev->mappedData(m_readback)) {
            m_lastCount = 0; m_lastWanted = 0; m_over = false;
            for (int r = 0; r < kRegions; ++r) {
                m_lastCount  += rb[r * 5 + 1];
                m_lastWanted += rb[5 * kRegions + r];
                m_over = m_over || rb[5 * kRegions + r] > m_cfg.capacity;
            }
        }
    }

    // ── EL PLAN ──────────────────────────────────────────────────────────────────────────────
    const double R = f.planetR;
    const uint32_t N = shellScatterCells(R + 0.5 * (f.shellLoM + f.shellHiM), m_cfg.cellM);
    const double cellM = (R + 0.5 * (f.shellLoM + f.shellHiM)) * 1.5707963267948966 / (double)N;
    const float focalPx = (float)(0.5 * (double)f.heightPx / std::max((double)f.tanHalfV, 1e-3));
    m_pixAngle = 1.0f / std::max(focalPx, 1.0f);
    const float cull = cullPx();
    ShellScatterPlanInput pin;
    pin.camPos = f.camPos; pin.planetC = f.planetC; pin.planetR = R; pin.rot = f.rot;
    pin.cells = N; pin.shellLoM = f.shellLoM; pin.shellHiM = f.shellHiM;
    // Alcance: el horizonte de la cáscara, y no más allá de donde el racimo más ancho (~3 celdas)
    // mide `cull` píxeles.
    const double pxReach = 3.0 * cellM * (double)focalPx / std::max((double)cull, 1e-3);
    pin.reachM = std::min({ shellHorizonM(R, glm::length(f.camPos - f.planetC), f.shellHiM), pxReach, m_cfg.maxReachM });
    pin.spillM = 3.0 * cellM;
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
    ShellScatterPlan plan;
    { HARUKA_PROFILE("shell.scatter.plan"); plan = planShellScatter(pin); }
    m_lastReach = pin.reachM;
    m_lastUsed = LastUsed{ pin, N, cellM, focalPx, pxPerBlob(), cull };
    if (plan.clipped) m_over = true;
    if (plan.work.empty()) return false;

    // ── LOS BUFFERS DEL FRAME ────────────────────────────────────────────────────────────────
    const size_t bytes = plan.work.size() * sizeof(ShellScatterBlock);
    if (m_workCap < bytes) {
        if (valid(m_work)) m_dev->destroy(m_work);
        m_workCap = bytes + bytes / 2 + 1024;
        m_work = m_dev->createBuffer(RHI::BufferUsage::Storage, m_workCap, nullptr, RHI::BufferMemory::Dynamic);
    }
    if (!valid(m_work)) return false;
    m_dev->updateBuffer(m_work, 0, bytes, plan.work.data());

    ScatterUBO u{};
    u.rot = glm::mat4(glm::mat3(glm::mat3_cast(f.rot)));
    const glm::dvec3 relC = f.planetC - f.camPos;
    u.center = glm::vec4(glm::vec3(relC), (float)R);
    float seedF; std::memcpy(&seedF, &m_cfg.seed, sizeof(seedF));
    u.grid = glm::vec4((float)N, (float)cellM, seedF, std::clamp(f.blend, 0.0f, 1.0f));
    u.px   = glm::vec4(focalPx, pxPerBlob() / std::max(m_cfg.pxPerBlobMin, 1e-3f), cull, 0.0f);
    u.lod  = glm::vec4(m_cfg.lodPx0, m_cfg.lodPx1, m_cfg.tint, m_cfg.darkTint / std::max(m_cfg.tint, 1e-3f));
    for (int i = 0; i < 4; ++i) u.plane[i] = planes[i];
    uint32_t lod2Bits; std::memcpy(&lod2Bits, &m_cfg.lodPx2, 4);
    u.cap = glm::uvec4(m_cfg.capacity, lod2Bits, (uint32_t)kVariants, 0u);
    u.shape = glm::vec4(m_cfg.aspectMul, m_cfg.stretch, 0.0f, 0.0f);
    u.wind  = glm::vec4(f.windDir, 0.0f);
    m_windDir = f.windDir;
    m_dev->updateBuffer(m_ubo, 0, sizeof(u), &u);

    uint32_t cmd0[kCmdWords] = {};
    for (int r = 0; r < kRegions; ++r) {
        cmd0[r * 5 + 0] = m_meshIdx[r];
        cmd0[r * 5 + 4] = (uint32_t)r * m_cfg.capacity;   // baseInstance: la región en el buffer
    }
    m_dev->updateBuffer(m_cmd, 0, sizeof(cmd0), cmd0);

    ctx->bindPipeline(m_pipe);
    ctx->bindUniformBuffer(0, m_ubo);
    ctx->bindStorageBuffer(3, m_inst);
    ctx->bindStorageBuffer(6, m_vol);
    ctx->bindStorageBuffer(4, m_cmd);
    ctx->bindStorageBuffer(5, m_work);
    ctx->bindTexture(1, valid(f.field) ? f.field : m_dummy);
    ctx->bindTexture(2, valid(f.fieldPrev) ? f.fieldPrev : (valid(f.field) ? f.field : m_dummy));
    ctx->dispatch((uint32_t)plan.work.size(), 1, 1);
    ctx->memoryBarrier();   // el dibujo lee instancias (vértices) y comandos (indirecto)
    m_lastWork = (uint32_t)plan.work.size();
    return true;
}

void ShellScatter::draw(RHI::Context* ctx, RHI::BufferHandle perFrameUBO, RHI::PipelineHandle restorePSO, float time) {
    if (!m_ready || !ctx || m_lastWork == 0) return;
    HARUKA_PROFILE("scene.shell"); HARUKA_GPU_SCOPE("scene.shell");
    PropParams pp{};
    pp.time = time;
    pp.matPBR = glm::vec4(0.0f, 1.0f, 1.0f, 0.0f);           // mate, sin texturas
    pp.material = glm::vec4(m_cfg.opacity, m_cfg.translucency, m_cfg.multi, m_cfg.wrap);   // prop_vol.frag: opacidad · contraluz · múltiple · rodeo
    const RHI::Device* dev = RHI::device();
    pp.flags = glm::vec4(dev && dev->backend() == RHI::Backend::Vulkan ? 1.0f : 0.0f, m_cfg.rim, m_cfg.paint, m_cfg.brushPx);
    pp.weather = glm::vec4(m_pixAngle, m_cfg.alpha, m_cfg.windStrokes ? 1.0f : 0.0f, 0.0f);   // prop_vol.frag: píxel · cuánto tapa · trazos con el viento
    pp.wind = m_windDir;
    m_dev->updateBuffer(m_params, 0, sizeof(pp), &pp);
    ctx->bindPipeline(m_drawPSO);
    ctx->bindUniformBuffer(0, perFrameUBO);
    ctx->bindUniformBuffer(6, m_params);
    ctx->bindVertexBuffer(m_inst, 1);
    ctx->bindVertexBuffer(m_vol, 2);
    for (int r = 0; r < kRegions; ++r) {
        if (m_meshIdx[r] == 0) continue;
        ctx->bindVertexBuffer(m_vbo[r], 0);
        ctx->bindIndexBuffer(m_ebo[r]);
        ctx->drawIndexedIndirect(m_cmd, 1, 20u, (size_t)r * 20u);
    }
    if (valid(restorePSO)) ctx->bindPipeline(restorePSO);
}

uint32_t ShellScatter::readBack(std::vector<uint32_t>& cmdOut, std::vector<std::vector<uint32_t>>& inst, uint32_t maxPerRegion) {
    cmdOut.clear(); inst.assign(kRegions, {});
    if (!m_ready) return 0;
    auto copyAndWait = [&](RHI::BufferHandle src, RHI::BufferHandle dst, size_t srcOff, size_t bytes) {
        RHI::FenceHandle fence{};
        if (RHI::Context* c = m_dev->beginFrame()) {
            c->memoryBarrier();
            m_dev->copyBuffer(src, dst, srcOff, 0, bytes);
            c->memoryBarrier();
            fence = c->signalFence();
            m_dev->endFrame();
        }
        if (RHI::Context* c2 = m_dev->beginFrame()) {
            if (valid(fence)) { c2->waitFence(fence, 5000000000ull); c2->deleteFence(fence); }
            m_dev->endFrame();
        }
    };
    copyAndWait(m_cmd, m_readback, 0, kCmdWords * 4u);
    if (const uint32_t* rb = (const uint32_t*)m_dev->mappedData(m_readback)) cmdOut.assign(rb, rb + kCmdWords);
    uint32_t total = 0;
    for (int r = 0; r < kRegions && cmdOut.size() == kCmdWords; ++r) {
        const uint32_t count = cmdOut[r * 5 + 1];
        total += count;
        const uint32_t take = std::min({ count, maxPerRegion, m_cfg.capacity });
        if (take == 0) continue;
        RHI::BufferHandle tmp = m_dev->createBuffer(RHI::BufferUsage::Storage, (size_t)take * kInstWords * 4u, nullptr, RHI::BufferMemory::Readback);
        if (!valid(tmp)) continue;
        copyAndWait(m_inst, tmp, (size_t)r * m_cfg.capacity * kInstWords * 4u, (size_t)take * kInstWords * 4u);
        if (const uint32_t* p = (const uint32_t*)m_dev->mappedData(tmp)) inst[r].assign(p, p + (size_t)take * kInstWords);
        m_dev->destroy(tmp);
    }
    return total;
}

} // namespace Haruka::World::Props
