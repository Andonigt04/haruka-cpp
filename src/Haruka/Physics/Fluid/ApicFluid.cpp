#include "Haruka/Physics/Fluid/ApicFluid.h"

#include "Haruka/RHI/RHIContext.h"
#include "Haruka/RHI/RHIDevice.h"
#include "Haruka/Renderer/Shader.h"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <string>

namespace Haruka::Physics::Fluid {

namespace {
// Mismo orden que `apic_fluid.comp`.
enum Pass { kTick = 0, kClear, kP2G, kMark, kFaces, kDiv, kPress, kProject, kG2P, kNodeClear, kSplat, kField };
// Punto fijo del P2G: 2^20. Una cara recibe Σw ≈ partículas por celda (8) y |v| de pocos m/s, así
// que la suma cabe en int32 con dos órdenes de margen; la resolución (1e-6) sobra.
constexpr float kFixedScale = 1048576.0f;

struct ParamsUBO { glm::ivec4 dims; glm::vec4 grid, wall, misc, force, sponge; };
struct PassUBO   { glm::ivec4 pass; };
struct GpuParticle { glm::vec4 x, v, cu, cv, cw; };

uint32_t hashU(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
float rand01(uint32_t& s) { s = hashU(s + 0x9e3779b9u); return (float)(s >> 8) * (1.0f / 16777216.0f); }
} // namespace

bool ApicFluid::init(RHI::Device* dev, const Config& cfg) {
    shutdown();
    if (!dev || cfg.nx < 3 || cfg.ny < 3 || cfg.nz < 1) return false;
    m_dev = dev; m_cfg = cfg;
    const int nx = cfg.nx, ny = cfg.ny, nz = cfg.nz;
    if ((int)m_cfg.bed.size() != nx * nz) m_cfg.bed.assign((size_t)nx * nz, 0.0f);

    // Siembra: 2×2×2 por celda con jitter, en las celdas líquidas (centro entre lecho y llenado,
    // delante de la pared en t = 0).
    std::vector<GpuParticle> parts;
    const float dx = cfg.dx;
    const float wall0 = 0.5f * cfg.wallStroke * (1.0f + std::tanh((0.0f - cfg.wallT0) / cfg.wallTau));
    const int sub = cfg.perCell >= 8 ? 2 : 1;
    uint32_t seed = 12345u;
    for (int k = 0; k < nz; ++k) for (int j = 0; j < ny; ++j) for (int i = 0; i < nx; ++i) {
        const float xc = (i + 0.5f) * dx, yc = (j + 0.5f) * dx;
        if (yc < m_cfg.bed[(size_t)k * nx + i] || yc > cfg.fillLevel || xc < wall0) continue;
        for (int a = 0; a < sub; ++a) for (int b = 0; b < sub; ++b) for (int c = 0; c < sub; ++c) {
            GpuParticle p{};
            const float h = dx / sub;
            p.x = glm::vec4(i * dx + (a + rand01(seed)) * h, j * dx + (b + rand01(seed)) * h,
                            k * dx + (c + rand01(seed)) * h, 1.0f);
            if (p.x.y > cfg.fillLevel) continue;
            parts.push_back(p);
        }
    }
    m_numParticles = (int)parts.size();
    if (m_numParticles == 0) return false;

    const std::string cs = Renderer::Shader::baseDir() + "shaders/apic_fluid.comp";
    { RHI::PipelineDesc pd; pd.computePath = cs.c_str(); m_pipe = dev->createPipeline(pd); }
    if (!RHI::valid(m_pipe)) { shutdown(); return false; }

    m_perCellSeeded = sub * sub * sub;
    m_params = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(ParamsUBO), nullptr, RHI::BufferMemory::Dynamic);
    m_solids = dev->createBuffer(RHI::BufferUsage::Storage, kMaxSpheres * 2 * sizeof(glm::vec4), nullptr, RHI::BufferMemory::Dynamic);
    m_wind = glm::vec2(0.0f); m_numSpheres = 0;
    uploadParams();
    // Un UBO por (pase, color), fijos: cambiar UNO entre dispatches del mismo frame no vale en
    // Vulkan (el command buffer lee la memoria al ejecutarse, todos verían el último valor).
    for (int p = 0; p < kPassUbos; ++p) {
        const bool second = p == kPassUbos - 1;
        PassUBO q{ glm::ivec4(second ? (int)kPress : p, second ? 1 : 0, 0, 0) };
        m_passUbo[p] = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(q), &q, RHI::BufferMemory::Static);
    }

    const size_t cells = (size_t)nx * ny * nz;
    const size_t faces = (size_t)(nx + 1) * ny * nz + (size_t)nx * (ny + 1) * nz + (size_t)nx * ny * (nz + (cfg.periodicZ ? 0 : 1));
    const std::vector<float> zeroF(std::max(cells, faces), 0.0f);
    const std::vector<uint32_t> zeroU(std::max(cells, 2 * faces), 0u);
    const glm::vec4 state(0.0f);
    m_particles = dev->createBuffer(RHI::BufferUsage::Storage, parts.size() * sizeof(GpuParticle), parts.data(), RHI::BufferMemory::Static);
    m_accum    = dev->createBuffer(RHI::BufferUsage::Storage, 2 * faces * 4, zeroU.data(), RHI::BufferMemory::Static);
    m_faces    = dev->createBuffer(RHI::BufferUsage::Storage, faces * 4, zeroF.data(), RHI::BufferMemory::Static);
    m_count    = dev->createBuffer(RHI::BufferUsage::Storage, cells * 4, zeroU.data(), RHI::BufferMemory::Static);
    m_type     = dev->createBuffer(RHI::BufferUsage::Storage, cells * 4, zeroU.data(), RHI::BufferMemory::Static);
    m_pressure = dev->createBuffer(RHI::BufferUsage::Storage, cells * 4, zeroF.data(), RHI::BufferMemory::Static);
    m_rhs      = dev->createBuffer(RHI::BufferUsage::Storage, cells * 4, zeroF.data(), RHI::BufferMemory::Static);
    m_bed      = dev->createBuffer(RHI::BufferUsage::Storage, m_cfg.bed.size() * 4, m_cfg.bed.data(), RHI::BufferMemory::Static);
    m_state    = dev->createBuffer(RHI::BufferUsage::Storage, sizeof(state), &state, RHI::BufferMemory::Static);
    const size_t nodes = (size_t)(nx + 1) * (ny + 1) * (nz + (cfg.periodicZ ? 0 : 1));
    const std::vector<uint32_t> zeroN(nodes, 0u);
    m_nodeAcc  = dev->createBuffer(RHI::BufferUsage::Storage, nodes * 4, zeroN.data(), RHI::BufferMemory::Static);
    m_field    = dev->createBuffer(RHI::BufferUsage::Storage, nodes * 4, zeroN.data(), RHI::BufferMemory::Static);
    m_time = 0.0;
    return true;
}

void ApicFluid::shutdown() {
    if (!m_dev) return;
    auto kill = [&](RHI::BufferHandle& b) { if (RHI::valid(b)) m_dev->destroy(b); b = {}; };
    kill(m_params); for (auto& b : m_passUbo) kill(b);
    kill(m_particles); kill(m_accum); kill(m_faces); kill(m_count); kill(m_type);
    kill(m_pressure); kill(m_rhs); kill(m_bed); kill(m_state); kill(m_nodeAcc); kill(m_field); kill(m_solids);
    if (RHI::valid(m_pipe)) m_dev->destroy(m_pipe);
    m_pipe = {};
    m_dev = nullptr;
    m_numParticles = 0;
}

void ApicFluid::uploadParams() {
    const ApicFluid::Config& c = m_cfg;
    ParamsUBO pu{};
    pu.dims = glm::ivec4(c.nx, c.ny, c.nz, m_numParticles);
    pu.grid = glm::vec4(c.dx, c.dt, c.gravity, (float)m_perCellSeeded);
    // Pared quieta: τ no puede ser 0 en el tanh; con stroke 0 la posición es 0 igual.
    pu.wall = glm::vec4(c.wallStroke, c.wallT0, c.wallTau > 0.0f ? c.wallTau : 1.0f, c.densityGain);
    pu.misc = glm::vec4(kFixedScale, c.sor, c.nx * c.dx, c.ny * c.dx);
    pu.force = glm::vec4(m_wind.x, 0.0f, m_wind.y, c.periodicZ ? 1.0f : 0.0f);
    pu.sponge = glm::vec4(c.spongeWidth, c.spongeRate, (float)m_numSpheres, 0.0f);
    m_dev->updateBuffer(m_params, 0, sizeof(pu), &pu);
}

void ApicFluid::setForcing(const glm::vec2& windAccel, const std::vector<Sphere>& spheres) {
    if (!m_dev || !RHI::valid(m_params)) return;
    m_wind = windAccel;
    m_numSpheres = std::min((int)spheres.size(), kMaxSpheres);
    if (m_numSpheres > 0) {
        glm::vec4 data[kMaxSpheres * 2];
        for (int i = 0; i < m_numSpheres; ++i) {
            data[2 * i]     = glm::vec4(spheres[(size_t)i].center, spheres[(size_t)i].radius);
            data[2 * i + 1] = glm::vec4(spheres[(size_t)i].velocity, 0.0f);
        }
        m_dev->updateBuffer(m_solids, 0, (size_t)m_numSpheres * 2 * sizeof(glm::vec4), data);
    }
    uploadParams();
}

void ApicFluid::dispatchPass(RHI::Context* ctx, int pass, int color, uint32_t threads) {
    ctx->bindUniformBuffer(1, m_passUbo[pass == kPress && color == 1 ? kPassUbos - 1 : pass]);
    ctx->dispatch((threads + 127u) / 128u, 1, 1);
    ctx->memoryBarrier();
}

void ApicFluid::step(RHI::Context* ctx, int steps) {
    if (!ctx || !RHI::valid(m_pipe)) return;
    const uint32_t nx = m_cfg.nx, ny = m_cfg.ny, nz = m_cfg.nz;
    const uint32_t cells = nx * ny * nz;
    const uint32_t faces = (nx + 1) * ny * nz + nx * (ny + 1) * nz + nx * ny * (nz + (m_cfg.periodicZ ? 0u : 1u));
    bindAll(ctx);
    for (int s = 0; s < steps; ++s) {
        dispatchPass(ctx, kTick, 0, 1);
        dispatchPass(ctx, kClear, 0, std::max(cells, 2 * faces));
        dispatchPass(ctx, kP2G, 0, (uint32_t)m_numParticles);
        dispatchPass(ctx, kMark, 0, cells);
        dispatchPass(ctx, kFaces, 0, faces);
        dispatchPass(ctx, kDiv, 0, cells);
        for (int it = 0; it < m_cfg.pressureIters; ++it) {
            dispatchPass(ctx, kPress, 0, cells);
            dispatchPass(ctx, kPress, 1, cells);
        }
        dispatchPass(ctx, kProject, 0, faces);
        dispatchPass(ctx, kG2P, 0, (uint32_t)m_numParticles);
        m_time += m_cfg.dt;
    }
}

void ApicFluid::bindAll(RHI::Context* ctx) {
    ctx->bindPipeline(m_pipe);
    ctx->bindUniformBuffer(0, m_params);
    ctx->bindStorageBuffer(2, m_particles);
    ctx->bindStorageBuffer(3, m_accum);
    ctx->bindStorageBuffer(4, m_faces);
    ctx->bindStorageBuffer(5, m_count);
    ctx->bindStorageBuffer(6, m_type);
    ctx->bindStorageBuffer(7, m_pressure);
    ctx->bindStorageBuffer(8, m_rhs);
    ctx->bindStorageBuffer(9, m_bed);
    ctx->bindStorageBuffer(10, m_state);
    ctx->bindStorageBuffer(11, m_nodeAcc);
    ctx->bindStorageBuffer(12, m_field);
    ctx->bindStorageBuffer(13, m_solids);
}

void ApicFluid::buildField(RHI::Context* ctx) {
    if (!ctx || !RHI::valid(m_pipe)) return;
    const glm::ivec3 fd = fieldDims();
    const uint32_t nodes = (uint32_t)(fd.x * fd.y * fd.z);
    bindAll(ctx);
    dispatchPass(ctx, kNodeClear, 0, nodes);
    dispatchPass(ctx, kSplat, 0, (uint32_t)m_numParticles);
    dispatchPass(ctx, kField, 0, nodes);
}

bool ApicFluid::readParticles(std::vector<ParticleState>& out) {
    out.clear();
    if (!m_dev || m_numParticles == 0) return false;
    const size_t bytes = (size_t)m_numParticles * sizeof(GpuParticle);
    RHI::BufferHandle tmp = m_dev->createBuffer(RHI::BufferUsage::Storage, bytes, nullptr, RHI::BufferMemory::Readback);
    if (!RHI::valid(tmp)) return false;
    RHI::FenceHandle fence{};
    if (RHI::Context* c = m_dev->beginFrame()) {
        c->memoryBarrier();
        m_dev->copyBuffer(m_particles, tmp, 0, 0, bytes);
        c->memoryBarrier();
        fence = c->signalFence();
        m_dev->endFrame();
    }
    if (RHI::Context* c2 = m_dev->beginFrame()) {
        if (RHI::valid(fence)) { c2->waitFence(fence, 10000000000ull); c2->deleteFence(fence); }
        m_dev->endFrame();
    }
    const GpuParticle* p = (const GpuParticle*)m_dev->mappedData(tmp);
    if (p) {
        out.resize((size_t)m_numParticles);
        for (int i = 0; i < m_numParticles; ++i) out[(size_t)i] = { glm::vec3(p[i].x), glm::vec3(p[i].v) };
    }
    m_dev->destroy(tmp);
    return p != nullptr;
}

} // namespace Haruka::Physics::Fluid
