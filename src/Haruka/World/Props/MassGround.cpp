#include "Haruka/World/Props/MassGround.h"

#include "Haruka/Core/Logger.h"
#include "Haruka/Renderer/Shader.h"
#include "Haruka/RHI/RHIContext.h"
#include "Haruka/RHI/RHIDevice.h"
#include "Haruka/RHI/RHIGPUScope.h"
#include "Haruka/Tools/Profiler.h"
#include "Haruka/World/Planet/GroundView.h"
#include "Haruka/World/Props/GroundScatter.h"   // buildGroundViewIndex
#include "Haruka/World/Terrain/CubeSphere.h"
#include "Haruka/World/Terrain/TerrainNode.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace Haruka::World::Props {

using RHI::valid;

namespace {
constexpr int kIdxDim = 32;   ///< lado de la rejilla del índice de nodos (el de la cubierta)

/// Gemelo std140 del UBO `MassGround` de mass_ground.comp. El `dvec4` va alineado a 32 B.
struct MassGroundUBO {
    glm::ivec4 nodeGrid, idxGrid;
    glm::dvec4 idx;
    glm::vec4  misc;
    glm::vec4  pad;
};
static_assert(offsetof(MassGroundUBO, idx) == 32, "dvec4 de MassGround alineado a 32 B");
} // namespace

std::vector<MassGroundJob> planMassGround(const MassGroundPlanInput& in, uint32_t& totalOut) {
    totalOut = 0;
    std::vector<MassGroundJob> jobs;
    if (!in.bursts || in.planetR <= 0.0 || in.budget == 0) return jobs;
    const std::vector<MassBurst>& bursts = *in.bursts;

    // Los candidatos, por distancia del borde de su celda a la cámara.
    std::vector<std::pair<double, uint32_t>> cand;
    for (uint32_t i = 0; i < (uint32_t)bursts.size(); ++i) {
        const MassBurst& b = bursts[i];
        if (b.count == 0) continue;
        const glm::dvec3 c = in.planetC + glm::normalize(glm::dvec3(b.dir)) * (in.planetR + (double)b.heightM);
        const double d = glm::length(c - in.camPos) - 0.65 * (double)b.cellM;
        if (d <= in.reachM) cand.push_back({ d, i });
    }
    std::sort(cand.begin(), cand.end());

    for (const auto& [d, i] : cand) {
        const MassBurst& b = bursts[i];
        const uint32_t n = std::min(b.count, kMassMaxCount);
        if (totalOut + n > in.budget) break;   // los más cercanos ya entraron: el resto, en su plano
        const glm::dvec3 dirD = glm::normalize(glm::dvec3(b.dir));
        Tools::PlanetFace face; double lx, ly;
        Terrain::dirToCubeFaceClosed(dirD, face, lx, ly);
        // El jacobiano del plano del brote, por diferencias centradas de ±1 m con las MISMAS tangentes
        // que el shader (`massTangents`, en float como allí).
        glm::vec3 u, v;
        massTangents(b.dir, u, v);
        const glm::dvec3 c = dirD * (in.planetR + (double)b.heightM);
        auto at = [&](const glm::dvec3& p, double& ox, double& oy) {
            Tools::PlanetFace f2; Terrain::dirToCubeFaceClosed(glm::normalize(p), f2, ox, oy);
            return f2 == face;
        };
        double xu0, yu0, xu1, yu1, xv0, yv0, xv1, yv1;
        if (!at(c - glm::dvec3(u), xu0, yu0) || !at(c + glm::dvec3(u), xu1, yu1) ||
            !at(c - glm::dvec3(v), xv0, yv0) || !at(c + glm::dvec3(v), xv1, yv1))
            continue;   // en una arista de cara: se queda en su plano
        MassGroundJob j{};
        j.a[0] = i; j.a[1] = totalOut; j.a[2] = n; j.a[3] = (uint32_t)face;
        j.l[0] = lx; j.l[1] = ly;
        j.j[0] = (float)((xu1 - xu0) * 0.5); j.j[1] = (float)((xv1 - xv0) * 0.5);
        j.j[2] = (float)((yu1 - yu0) * 0.5); j.j[3] = (float)((yv1 - yv0) * 0.5);
        j.m[0] = b.heightM;
        jobs.push_back(j);
        totalOut += n;
    }
    return jobs;
}

bool MassGround::init(RHI::Device* dev) {
    shutdown();
    if (!dev) return false;
    m_dev = dev;
    const std::string cs = Renderer::Shader::baseDir() + "shaders/mass_ground.comp";
    { RHI::PipelineDesc pd; pd.computePath = cs.c_str(); m_pipe = dev->createPipeline(pd); }
    m_ubo = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(MassGroundUBO), nullptr, RHI::BufferMemory::Dynamic);
    // Sellos y ajuste SIEMPRE atables (el vertex los declara): a cero, que no coincide con ningún frame.
    const uint32_t zeros[4] = { 0u, 0u, 0u, 0u };
    m_stamps = dev->createBuffer(RHI::BufferUsage::Storage, sizeof(zeros), zeros, RHI::BufferMemory::Dynamic);
    m_snap   = dev->createBuffer(RHI::BufferUsage::Storage, sizeof(zeros), zeros, RHI::BufferMemory::Dynamic);
    m_stampsCap = m_snapCap = sizeof(zeros);
    // Sin compute los sellos (a cero) siguen atables: el vertex no los usa, pero los declara.
    m_ready = valid(m_pipe) && valid(m_ubo) && valid(m_stamps) && valid(m_snap);
    if (!m_ready)
        HARUKA_LOGW("MassGround", "no se pudo crear (compute %s) — los brotes se quedan en su plano",
                    valid(m_pipe) ? "ok" : "FALLO");
    return m_ready;
}

void MassGround::shutdown() {
    if (m_dev) {
        for (RHI::BufferHandle* b : { &m_ubo, &m_nodes, &m_jobs, &m_snap, &m_stamps, &m_idxOff, &m_idxList })
            if (valid(*b)) m_dev->destroy(*b);
        if (valid(m_pipe)) m_dev->destroy(m_pipe);
    }
    m_ubo = m_nodes = m_jobs = m_snap = m_stamps = m_idxOff = m_idxList = {};
    m_pipe = {};
    m_nodesCap = m_jobsCap = m_snapCap = m_stampsCap = m_idxOffCap = m_idxListCap = 0;
    m_ready = false; m_dev = nullptr; m_frame = 0;
}

bool MassGround::dispatch(RHI::Context* ctx, const Planet::GroundView& g, const glm::dvec3& camPos, double reachM,
                          RHI::BufferHandle bursts, size_t burstCount, const std::vector<MassGroundJob>& jobs,
                          uint32_t totalElems, float sinkM) {
    if (!m_ready || !ctx) return false;
    // El frame avanza SIEMPRE: si este no ajusta nada, los sellos del anterior dejan de valer.
    // Cabe exacto en el float de `u_flags.w` hasta 2^24; se da la vuelta antes, saltando el 0.
    m_frame = (m_frame % 0xFFFFFFu) + 1u;
    if (jobs.empty() || totalElems == 0 || !valid(bursts) || burstCount == 0 || g.nodes.empty()) return false;
    HARUKA_PROFILE("mass.ground"); HARUKA_GPU_SCOPE("mass.ground");

    // Se reasignan al crecer (el frame anterior ya terminó: el device espera en endFrame).
    auto put = [&](RHI::BufferHandle& buf, size_t& cap, const void* data, size_t bytes) {
        if (cap < bytes) {
            if (valid(buf)) m_dev->destroy(buf);
            cap = bytes + bytes / 2 + 1024;
            buf = m_dev->createBuffer(RHI::BufferUsage::Storage, cap, nullptr, RHI::BufferMemory::Dynamic);
        }
        if (valid(buf) && data && bytes > 0) m_dev->updateBuffer(buf, 0, bytes, data);
    };
    // Los sellos: uno por brote. Al crecer se rellenan a CERO (un sello basura podría coincidir).
    if (m_stampsCap < burstCount * 8u) {
        if (valid(m_stamps)) m_dev->destroy(m_stamps);
        m_stampsCap = burstCount * 8u + burstCount * 4u + 1024u;
        const std::vector<uint32_t> z(m_stampsCap / 4u, 0u);
        m_stamps = m_dev->createBuffer(RHI::BufferUsage::Storage, m_stampsCap, z.data(), RHI::BufferMemory::Dynamic);
    }
    put(m_snap, m_snapCap, nullptr, (size_t)totalElems * 4u);
    put(m_jobs, m_jobsCap, jobs.data(), jobs.size() * sizeof(MassGroundJob));
    put(m_nodes, m_nodesCap, g.nodes.data(), g.nodes.size() * sizeof(g.nodes[0]));
    const GroundViewIndex gi = buildGroundViewIndex(g, camPos, reachM, kIdxDim);
    put(m_idxOff, m_idxOffCap, gi.off.data(), gi.off.size() * 4u);
    put(m_idxList, m_idxListCap, gi.list.data(), gi.list.size() * 4u);
    if (!valid(m_snap) || !valid(m_jobs) || !valid(m_nodes) || !valid(m_idxOff) || !valid(m_idxList) || !valid(m_stamps))
        return false;

    MassGroundUBO u{};
    u.nodeGrid = glm::ivec4((int)Terrain::TERRAIN_NODE_TEXELS, (int)Terrain::TERRAIN_NODE_CELLS,
                            (int)g.nodes.size(), (int)m_frame);
    u.idxGrid  = glm::ivec4(kIdxDim, kIdxDim, (int)jobs.size(), 0);
    u.idx      = glm::dvec4(gi.x0, gi.y0, gi.cell, gi.span);
    u.misc     = glm::vec4(sinkM, 0.0f, 0.0f, 0.0f);
    m_dev->updateBuffer(m_ubo, 0, sizeof(u), &u);

    ctx->bindPipeline(m_pipe);
    ctx->bindUniformBuffer(0, m_ubo);
    ctx->bindStorageBuffer(1, m_nodes);
    ctx->bindStorageBuffer(2, g.heights);
    ctx->bindStorageBuffer(3, bursts);
    ctx->bindStorageBuffer(4, m_jobs);
    ctx->bindStorageBuffer(5, m_snap);
    ctx->bindStorageBuffer(6, m_idxOff);
    ctx->bindStorageBuffer(7, m_idxList);
    ctx->bindStorageBuffer(8, m_stamps);
    ctx->dispatch((uint32_t)jobs.size(), 1, 1);
    ctx->memoryBarrier();   // el vertex de los brotes lee el ajuste y los sellos
    return true;
}

void MassGround::readBack(size_t elems, size_t bursts, std::vector<float>& snapOut, std::vector<uint32_t>& stampsOut) {
    snapOut.clear(); stampsOut.clear();
    if (!m_dev) return;
    // Mismo patrón que `GroundScatter::readBack`: frame con copia + barrera + fence, y otro que la espera.
    auto copy = [&](RHI::BufferHandle src, size_t bytes, auto& out) {
        if (!valid(src) || bytes == 0) return;
        RHI::BufferHandle tmp = m_dev->createBuffer(RHI::BufferUsage::Storage, bytes, nullptr, RHI::BufferMemory::Readback);
        if (!valid(tmp)) return;
        RHI::FenceHandle fence{};
        if (RHI::Context* c = m_dev->beginFrame()) {
            c->memoryBarrier();
            m_dev->copyBuffer(src, tmp, 0, 0, bytes);
            c->memoryBarrier();
            fence = c->signalFence();
            m_dev->endFrame();
        }
        if (RHI::Context* c2 = m_dev->beginFrame()) {
            if (valid(fence)) { c2->waitFence(fence, 5000000000ull); c2->deleteFence(fence); }
            m_dev->endFrame();
        }
        using T = typename std::decay_t<decltype(out)>::value_type;
        if (const T* p = (const T*)m_dev->mappedData(tmp)) out.assign(p, p + bytes / sizeof(T));
        m_dev->destroy(tmp);
    };
    copy(m_snap, std::min(elems * 4u, m_snapCap), snapOut);
    copy(m_stamps, std::min(bursts * 8u, m_stampsCap), stampsOut);
}

} // namespace Haruka::World::Props
