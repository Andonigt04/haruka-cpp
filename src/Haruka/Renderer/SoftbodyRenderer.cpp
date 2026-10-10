#include "SoftbodyRenderer.h"
#include "Haruka/RHI/RHIDevice.h"
#include "Haruka/RHI/RHIContext.h"
#include "Haruka/Renderer/Shader.h"
#include <glm/glm.hpp>

namespace Haruka {

SoftBodyRenderer::~SoftBodyRenderer() { clear(); }

void SoftBodyRenderer::clear() {
    RHI::Device* dev = RHI::device();
    for (auto& e : m_entries) {
        if (dev) {
            if (RHI::valid(e.hVbo)) dev->destroy(e.hVbo);
            if (RHI::valid(e.hEbo)) dev->destroy(e.hEbo);
            if (RHI::valid(e.hParams)) dev->destroy(e.hParams);
        }
    }
    m_entries.clear();
    if (dev) {
        if (RHI::valid(m_pso))       { dev->destroy(m_pso);       m_pso       = {}; }
        if (RHI::valid(m_paramsUBO)) { dev->destroy(m_paramsUBO); m_paramsUBO = {}; }
    }
}

void SoftBodyRenderer::ensurePSO() {
    if (RHI::valid(m_pso)) return;
    RHI::Device* dev = RHI::device();
    if (!dev) return;

    // ⚠️ CON `Shader::baseDir()`, como todos los demás. Relativo a secas ("shaders/…") sólo lo
    // encontraba el backend GL; Vulkan lo buscaba desde el directorio de trabajo y creaba el pipeline
    // SIN shaders (lo dice el log: "pipeline sin vertex"), así que en Vulkan no se dibujaba nada.
    const std::string vs = Shader::baseDir() + "shaders/softbody.vert";
    const std::string fs = Shader::baseDir() + "shaders/softbody.frag";
    RHI::PipelineDesc pd;
    pd.vertexPath   = vs.c_str();
    pd.fragmentPath = fs.c_str();
    pd.cull         = RHI::CullMode::None;
    pd.depth.test   = true;
    pd.depth.write  = true;
    // ⚠️ SIN `compare`: el motor usa REVERSED-Z (profundidad borrada a 0, prueba Greater por defecto).
    // Aquí ponía `Less`, y contra un z-buffer borrado a 0 ningún fragmento pasa nunca: la bandera y la
    // gelatina de la demo no se han visto jamás, y la piel de las criaturas tampoco se veía.
    pd.topology     = RHI::PrimitiveTopology::Triangles;

    pd.vertexLayout.strides  = { 6 * sizeof(float) };
    pd.vertexLayout.attributes = {
        { 0, 0,                      RHI::Format::RGB32F, 0 }, // aPos
        { 1, 3 * sizeof(float),      RHI::Format::RGB32F, 0 }, // aNormal
    };

    m_pso = dev->createPipeline(pd);

    // SoftbodyParams UBO (binding 6): vec3 color + float alphaMode
    m_paramsUBO = dev->createBuffer(RHI::BufferUsage::Uniform, 16, nullptr, RHI::BufferMemory::Dynamic);
}

int SoftBodyRenderer::add(const Physics::XPBD::SoftBodyHandle& handle, Physics::XPBD::XPBDSolver* solver,
                          const glm::vec3& color) {
    Entry e;
    e.id = m_nextId++;
    e.handle = handle;
    e.solver = solver;
    e.color  = color;

    RHI::Device* dev = RHI::device();
    e.hEbo = dev->createBuffer(RHI::BufferUsage::Index, handle.renderIndices.size() * sizeof(unsigned int),
                               handle.renderIndices.data());
    e.hVbo = dev->createBuffer(RHI::BufferUsage::Vertex, 0, nullptr, RHI::BufferMemory::Stream);
    const float params[4] = { color.x, color.y, color.z, 0.0f };   // vec3 color + alphaMode = 0
    e.hParams = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(params), params, RHI::BufferMemory::Dynamic);

    e.initialized = true;
    const int id = e.id;
    m_entries.push_back(std::move(e));
    return id;
}

void SoftBodyRenderer::remove(int id) {
    RHI::Device* dev = RHI::device();
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it->id != id) continue;
        if (dev) {
            if (RHI::valid(it->hVbo))    dev->destroy(it->hVbo);
            if (RHI::valid(it->hEbo))    dev->destroy(it->hEbo);
            if (RHI::valid(it->hParams)) dev->destroy(it->hParams);
        }
        m_entries.erase(it);
        return;
    }
}

void SoftBodyRenderer::uploadEntry(Entry& e, const Haruka::WorldPos& cameraPos) {
    auto& ps = e.solver->particles;
    const int base = e.handle.firstParticle;
    const int cnt  = e.handle.particleCount;

    std::vector<glm::vec3> camPos(cnt);
    for (int i = 0; i < cnt; ++i)
        camPos[i] = glm::vec3(ps.worldPos(base + i) - glm::dvec3(cameraPos));

    std::vector<glm::vec3> nrm(cnt, glm::vec3(0.0f));
    const auto& idx = e.handle.renderIndices;
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        int ia = (int)idx[t]   - base;
        int ib = (int)idx[t+1] - base;
        int ic = (int)idx[t+2] - base;
        if (ia < 0 || ib < 0 || ic < 0 || ia >= cnt || ib >= cnt || ic >= cnt) continue;
        glm::vec3 fn = glm::cross(camPos[ib] - camPos[ia], camPos[ic] - camPos[ia]);
        nrm[ia] += fn; nrm[ib] += fn; nrm[ic] += fn;
    }

    e.cpuVerts.resize(cnt * 6);
    for (int i = 0; i < cnt; ++i) {
        glm::vec3 n = nrm[i];
        float l = glm::length(n);
        n = (l > 1e-8f) ? n / l : glm::vec3(0, 1, 0);
        e.cpuVerts[i*6+0] = camPos[i].x; e.cpuVerts[i*6+1] = camPos[i].y; e.cpuVerts[i*6+2] = camPos[i].z;
        e.cpuVerts[i*6+3] = n.x;         e.cpuVerts[i*6+4] = n.y;         e.cpuVerts[i*6+5] = n.z;
    }

    if (RHI::Device* dev = RHI::device())
        dev->uploadBuffer(e.hVbo, e.cpuVerts.size() * sizeof(float), e.cpuVerts.data());
}

void SoftBodyRenderer::render(const Haruka::WorldPos& cameraPos) {
    if (m_entries.empty()) return;
    ensurePSO();
    if (!RHI::valid(m_pso) || !RHI::valid(m_paramsUBO)) return;

    RHI::Device* dev = RHI::device();
    if (!dev) return;
    RHI::Context* ctx = dev->beginFrame();

    ctx->bindPipeline(m_pso);

    for (auto& e : m_entries) {
        if (!e.initialized || !e.solver) continue;
        uploadEntry(e, cameraPos);
        ctx->bindUniformBuffer(6, e.hParams);

        ctx->bindVertexBuffer(e.hVbo, 0);
        ctx->bindIndexBuffer(e.hEbo);
        ctx->drawIndexed((uint32_t)e.handle.renderIndices.size());
    }
}

} // namespace Haruka