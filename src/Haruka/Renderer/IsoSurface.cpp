#include "Haruka/Renderer/IsoSurface.h"

#include "Haruka/RHI/RHIContext.h"
#include "Haruka/RHI/RHIDevice.h"
#include "Haruka/Renderer/Shader.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace Haruka::Renderer {

namespace {
enum Pass { kClear = 0, kVerts, kQuads, kFinish };
struct ParamsUBO { glm::ivec4 dims; glm::vec4 iso, origin; };
} // namespace

bool IsoSurface::init(RHI::Device* dev, uint32_t capacity) {
    shutdown();
    if (!dev || capacity < 3) return false;
    m_dev = dev;
    m_capacity = capacity - capacity % 3u;
    const std::string cs = Shader::baseDir() + "shaders/iso_surface.comp";
    { RHI::PipelineDesc pd; pd.computePath = cs.c_str(); m_pipe = dev->createPipeline(pd); }
    if (!RHI::valid(m_pipe)) { shutdown(); return false; }
    // El UBO de la rejilla cambia como mucho una vez por frame (al extraer); los de pase son fijos:
    // en Vulkan un UBO reescrito entre dispatches del mismo frame lo verían todos con el último valor.
    m_params = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(ParamsUBO), nullptr, RHI::BufferMemory::Dynamic);
    for (int p = 0; p < 4; ++p) {
        const glm::ivec4 q(p, 0, 0, 0);
        m_pass[p] = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(q), &q, RHI::BufferMemory::Static);
    }
    m_out = dev->createBuffer(RHI::BufferUsage::Storage, (size_t)m_capacity * sizeof(Vertex), nullptr, RHI::BufferMemory::Static);
    std::vector<uint32_t> ident(m_capacity);
    for (uint32_t i = 0; i < m_capacity; ++i) ident[i] = i;
    m_idx = dev->createBuffer(RHI::BufferUsage::Index, ident.size() * 4, ident.data(), RHI::BufferMemory::Static);
    const uint32_t cmd0[6] = {};
    m_cmd = dev->createBuffer(RHI::BufferUsage::Indirect, sizeof(cmd0), cmd0, RHI::BufferMemory::Dynamic);
    return RHI::valid(m_out) && RHI::valid(m_idx) && RHI::valid(m_cmd);
}

void IsoSurface::shutdown() {
    if (!m_dev) return;
    auto kill = [&](RHI::BufferHandle& b) { if (RHI::valid(b)) m_dev->destroy(b); b = {}; };
    kill(m_params); for (auto& b : m_pass) kill(b);
    kill(m_cell); kill(m_out); kill(m_idx); kill(m_cmd); kill(m_cmdRb);
    m_polled = false;
    if (RHI::valid(m_pipe)) m_dev->destroy(m_pipe);
    m_pipe = {};
    m_dev = nullptr;
    m_cells = 0;
}

void IsoSurface::ensureCells(size_t cells) {
    if (cells <= m_cells && RHI::valid(m_cell)) return;
    if (RHI::valid(m_cell)) m_dev->destroy(m_cell);
    m_cells = cells;
    m_cell = m_dev->createBuffer(RHI::BufferUsage::Storage, cells * sizeof(Vertex), nullptr, RHI::BufferMemory::Static);
}

void IsoSurface::extract(RHI::Context* ctx, RHI::BufferHandle field, const Grid& g) {
    if (!ctx || !RHI::valid(m_pipe) || !RHI::valid(field)) return;
    if (g.nodes.x < 2 || g.nodes.y < 2 || g.nodes.z < 1) return;
    const size_t cells = (size_t)(g.nodes.x - 1) * (g.nodes.y - 1) * (g.periodicZ ? g.nodes.z : g.nodes.z - 1);
    if (cells == 0) return;
    ensureCells(cells);
    ParamsUBO pu{ glm::ivec4(g.nodes, g.periodicZ ? 1 : 0), glm::vec4(g.cell, g.iso, (float)m_capacity, g.openSides ? 1.0f : 0.0f),
                  glm::vec4(g.origin, 0.0f) };
    m_dev->updateBuffer(m_params, 0, sizeof(pu), &pu);
    ctx->bindPipeline(m_pipe);
    ctx->bindUniformBuffer(0, m_params);
    ctx->bindStorageBuffer(2, field);
    ctx->bindStorageBuffer(3, m_cell);
    ctx->bindStorageBuffer(4, m_out);
    ctx->bindStorageBuffer(5, m_cmd);
    auto run = [&](int pass, uint32_t threads) {
        ctx->bindUniformBuffer(1, m_pass[pass]);
        ctx->dispatch((threads + 127u) / 128u, 1, 1);
        ctx->memoryBarrier();
    };
    run(kClear, 6);
    run(kVerts, (uint32_t)cells);
    run(kQuads, (uint32_t)cells);
    run(kFinish, 1);
}

void IsoSurface::draw(RHI::Context* ctx, RHI::PipelineHandle pipe, RHI::BufferHandle drawUBO) const {
    if (!ctx || !RHI::valid(pipe) || !RHI::valid(m_cmd)) return;
    ctx->bindPipeline(pipe);
    ctx->bindUniformBuffer(0, drawUBO);
    ctx->bindStorageBuffer(4, m_out);
    ctx->bindIndexBuffer(m_idx);
    ctx->drawIndexedIndirect(m_cmd, 1, kCommandStride);
}

int64_t IsoSurface::pollCount(RHI::Context* ctx) {
    if (!m_dev || !ctx || !RHI::valid(m_cmd)) return -1;
    if (!RHI::valid(m_cmdRb)) m_cmdRb = m_dev->createBuffer(RHI::BufferUsage::Storage, 24, nullptr, RHI::BufferMemory::Readback);
    int64_t last = -1;
    if (m_polled) if (const uint32_t* c = (const uint32_t*)m_dev->mappedData(m_cmdRb)) last = c[0];
    ctx->memoryBarrier();
    m_dev->copyBuffer(m_cmd, m_cmdRb, 0, 0, 24);
    m_polled = true;
    return last;
}

bool IsoSurface::readBack(std::vector<Vertex>& out, uint32_t& written) {
    out.clear(); written = 0;
    if (!m_dev) return false;
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
            if (RHI::valid(fence)) { c2->waitFence(fence, 10000000000ull); c2->deleteFence(fence); }
            m_dev->endFrame();
        }
    };
    RHI::BufferHandle rc = m_dev->createBuffer(RHI::BufferUsage::Storage, 24, nullptr, RHI::BufferMemory::Readback);
    copyAndWait(m_cmd, rc, 24);
    if (const uint32_t* c = (const uint32_t*)m_dev->mappedData(rc)) written = c[0];
    m_dev->destroy(rc);
    if (written == 0) return true;
    RHI::BufferHandle rv = m_dev->createBuffer(RHI::BufferUsage::Storage, (size_t)written * sizeof(Vertex), nullptr, RHI::BufferMemory::Readback);
    copyAndWait(m_out, rv, (size_t)written * sizeof(Vertex));
    const Vertex* v = (const Vertex*)m_dev->mappedData(rv);
    if (v) out.assign(v, v + written);
    m_dev->destroy(rv);
    return v != nullptr;
}

} // namespace Haruka::Renderer
