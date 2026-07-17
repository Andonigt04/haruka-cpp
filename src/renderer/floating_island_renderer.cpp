#include "renderer/floating_island_renderer.h"
#include "renderer/shader.h"      // Shader::baseDir (createPipeline NO resuelve rutas)
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"

namespace Haruka {

namespace {
    // Mismo layout std430 que el DrawItem de planet.vert (SSBO binding 7).
    struct GpuDrawItem { float offMorph[4]; int32_t mode[4]; };
    static_assert(sizeof(GpuDrawItem) == 32, "std430: vec4 + ivec4");
}

FloatingIslandRenderer::~FloatingIslandRenderer() {
    cleanup();
    if (RHI::Device* dev = RHI::device()) {
        if (RHI::valid(m_pso))  dev->destroy(m_pso);
        if (RHI::valid(m_ssbo)) dev->destroy(m_ssbo);
    }
    m_pso = {}; m_ssbo = {};
}

void FloatingIslandRenderer::cleanup() {
    if (RHI::valid(m_vboH)) {
        if (RHI::Device* dev = RHI::device()) { dev->destroy(m_vboH); dev->destroy(m_nboH); dev->destroy(m_eboH); }
        m_vboH = m_nboH = m_eboH = {};
    }
    m_indexCount = 0;
}

void FloatingIslandRenderer::setIslands(std::vector<FloatingIsland> islands) {
    m_pending  = std::move(islands);
    m_uploaded = false;
}

void FloatingIslandRenderer::uploadPending() {
    cleanup();
    m_islandCount = (int)m_pending.size();
    if (m_pending.empty()) { m_uploaded = true; return; }

    // Fusiona todas las islas en arrays únicos. Vértices relativos a un origen del
    // casquete → precisión float segura.
    m_origin = m_pending[0].worldCenter;
    std::vector<glm::vec3> verts, norms;
    std::vector<unsigned int> idx;
    for (const auto& isl : m_pending) {
        const auto& mesh = isl.mesh;
        if (mesh.vertices.empty() || mesh.indices.empty()) continue;
        glm::vec3 base = glm::vec3(isl.worldCenter - m_origin);
        unsigned int off = (unsigned int)verts.size();
        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            verts.push_back(base + mesh.vertices[i]);
            norms.push_back(mesh.normals[i]);
        }
        for (unsigned int ind : mesh.indices) idx.push_back(off + ind);
    }
    m_indexCount = (uint32_t)idx.size();
    if (m_indexCount == 0) { m_uploaded = true; m_pending.clear(); return; }

    RHI::Device* dev = RHI::device();
    m_vboH = dev->createBuffer(RHI::BufferUsage::Vertex, verts.size() * sizeof(glm::vec3), verts.data());
    m_nboH = dev->createBuffer(RHI::BufferUsage::Vertex, norms.size() * sizeof(glm::vec3), norms.data());
    m_eboH = dev->createBuffer(RHI::BufferUsage::Index,  idx.size()   * sizeof(unsigned int), idx.data());

    m_uploaded = true;
    m_pending.clear();
}

void FloatingIslandRenderer::render(const Haruka::WorldPos& cameraPos) {
    RHI::Device* dev = RHI::device();
    if (!dev) return;
    if (!m_uploaded) uploadPending();
    if (m_indexCount == 0) return;

    if (!RHI::valid(m_pso)) {
        // Mismos shaders que el terreno, pero layout PROPIO: posición y normal en streams vec3
        // float separados (las islas no traen normales empaquetadas ni morph). Los atributos que
        // el shader declara y este layout no alimenta (uv, morph) quedan sin atar → constante 0,
        // que es lo correcto: con morph = 0 el mix() es la identidad y la uv no se usa.
        const std::string vs = Shader::baseDir() + "shaders/planet.vert";
        const std::string fs = Shader::baseDir() + "shaders/planet.frag";
        RHI::PipelineDesc pd;
        pd.vertexPath   = vs.c_str();
        pd.fragmentPath = fs.c_str();
        pd.vertexLayout.strides    = { (uint32_t)sizeof(glm::vec3), (uint32_t)sizeof(glm::vec3) };
        pd.vertexLayout.attributes = {
            { 0, 0, RHI::Format::RGB32F, 0 },   // aPos
            { 1, 0, RHI::Format::RGB32F, 1 },   // aNormal
        };
        pd.depth.test    = true;
        pd.depth.write   = true;
        pd.depth.compare = RHI::CompareOp::GreaterEqual;   // reversed-Z, igual que el terreno
        pd.blend.enable  = false;
        pd.cull          = RHI::CullMode::None;         // a dos caras: el winding del blob varía
        m_pso  = dev->createPipeline(pd);
        m_ssbo = dev->createBuffer(RHI::BufferUsage::Storage, sizeof(GpuDrawItem), nullptr,
                                   RHI::BufferMemory::Dynamic);
    }

    glm::vec3 offset = glm::vec3(m_origin - glm::dvec3(cameraPos));
    GpuDrawItem it{};
    it.offMorph[0] = offset.x; it.offMorph[1] = offset.y; it.offMorph[2] = offset.z;
    it.offMorph[3] = 0.0f;   // las islas no morphan
    it.mode[0]     = 0;      // procedural
    dev->updateBuffer(m_ssbo, 0, sizeof(it), &it);

    RHI::Context* ctx = dev->beginFrame();
    ctx->bindPipeline(m_pso);
    ctx->bindStorageBuffer(7, m_ssbo);
    ctx->bindVertexBuffer(m_vboH, 0);
    ctx->bindVertexBuffer(m_nboH, 1);
    ctx->bindIndexBuffer(m_eboH);
    ctx->drawIndexed(m_indexCount);
}

} // namespace Haruka
