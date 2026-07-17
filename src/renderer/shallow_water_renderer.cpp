#include "shallow_water_renderer.h"
#include "shader.h"
#include "rhi/rhi_context.h"

namespace {
// std140 (binding 6): vec3 (align 16, size 12) + el float siguiente entran en el mismo slot de 16 B.
struct SoftbodyParams { glm::vec3 color; float alphaMode; };
static_assert(sizeof(SoftbodyParams) == 16, "SoftbodyParams std140 size mismatch");
}
#include "physics/fluid/shallow_water.h"
#include "rhi/rhi_device.h"
#include <glm/glm.hpp>

namespace Haruka {

ShallowWaterRenderer::~ShallowWaterRenderer() {
    if (RHI::Device* dev = RHI::device()) {
        if (RHI::valid(m_vboH)) dev->destroy(m_vboH);
        if (RHI::valid(m_eboH)) dev->destroy(m_eboH);
        if (RHI::valid(m_uboH)) dev->destroy(m_uboH);
        if (RHI::valid(m_pso))  dev->destroy(m_pso);
    }
}

void ShallowWaterRenderer::ensureGL() {
    if (m_init) return;
    RHI::Device* dev = RHI::device();
    if (!dev) return;
    // OJO: createPipeline hace un ifstream CRUDO → enraizar con el base dir de assets.
    const std::string vs = Shader::baseDir() + "shaders/softbody.vert";
    const std::string fs = Shader::baseDir() + "shaders/softbody.frag";
    RHI::PipelineDesc pd;
    pd.vertexPath          = vs.c_str();
    pd.fragmentPath        = fs.c_str();
    pd.vertexLayout.strides = { (uint32_t)(7 * sizeof(float)) };   // pos(3) + normal(3) + alpha(1)
    pd.vertexLayout.attributes = {
        { 0, 0,                 RHI::Format::RGB32F },
        { 1, 3 * sizeof(float), RHI::Format::RGB32F },
        { 2, 6 * sizeof(float), RHI::Format::R32F   },   // alpha por-vértice (fade de borde)
    };
    pd.topology     = RHI::PrimitiveTopology::Triangles;
    pd.depth.test   = true;  pd.depth.write = true;
    pd.blend.enable = true;  pd.blend.mode  = RHI::BlendMode::Alpha;
    pd.cull         = RHI::CullMode::None;        // agua a DOS CARAS (antes: glDisable(GL_CULL_FACE))
    m_pso  = dev->createPipeline(pd);
    m_vboH = dev->createBuffer(RHI::BufferUsage::Vertex,  0, nullptr, RHI::BufferMemory::Stream);
    m_eboH = dev->createBuffer(RHI::BufferUsage::Index,   0, nullptr, RHI::BufferMemory::Stream);
    m_uboH = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(SoftbodyParams), nullptr,
                               RHI::BufferMemory::Dynamic);
    m_init = true;
}

void ShallowWaterRenderer::render(const Haruka::WorldPos& cameraPos) {
    if (!m_sim) return;
    ensureGL();

    const int n = m_sim->size();
    if (n < 2) return;

    // Surface position of cell (i,j): anchor-plane world pos lifted by
    // (terrain + water) along the up axis. Camera-relative for float safety.
    const glm::dvec3 up = m_sim->up();
    auto surfPos = [&](int i, int j) -> glm::vec3 {
        glm::dvec3 base = m_sim->worldPosAt(i, j);
        double lift = double(m_sim->terrainAt(i,j)) + double(m_sim->waterAt(i,j));
        glm::dvec3 wp = base + up * lift;
        return glm::vec3(wp - glm::dvec3(cameraPos));
    };

    // Build vertex grid (only used cells matter, but a full grid keeps indexing
    // simple; cells without water collapse to terrain and are skipped in tris).
    m_verts.clear();
    m_verts.resize(size_t(n) * n * 7, 0.0f);
    std::vector<glm::vec3> pos(size_t(n) * n);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
            pos[j*n+i] = surfPos(i, j);

    // Smooth normals from neighbours.
    auto P = [&](int i,int j){ i=glm::clamp(i,0,n-1); j=glm::clamp(j,0,n-1); return pos[j*n+i]; };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            glm::vec3 dx = P(i+1,j) - P(i-1,j);
            glm::vec3 dz = P(i,j+1) - P(i,j-1);
            glm::vec3 nrm = glm::cross(dz, dx);
            float l = glm::length(nrm);
            nrm = (l > 1e-8f) ? nrm/l : glm::vec3(up);
            // Opacity fades with water depth so thin boundary water (the cells that
            // make the river look "square") dissolves into a smooth, depth-following
            // waterline instead of a hard grid edge. Deeper water reads as solid.
            float depth = m_sim->waterAt(i, j);
            float alpha = glm::smoothstep(0.02f, 0.30f, depth) * 0.85f;
            int b = (j*n+i)*7;
            m_verts[b+0]=pos[j*n+i].x; m_verts[b+1]=pos[j*n+i].y; m_verts[b+2]=pos[j*n+i].z;
            m_verts[b+3]=nrm.x; m_verts[b+4]=nrm.y; m_verts[b+5]=nrm.z;
            m_verts[b+6]=alpha;
        }

    // Triangles only for quads where at least one corner holds water POR ENCIMA del mar.
    // F5.3: las celdas a/bajo el nivel del mar las dibujan los chunks de océano (Gerstner) →
    // aquí se saltan para que no haya DOBLE lámina en la costa. Sin océano acoplado
    // (seaLevel=-1e9) se dibuja todo como antes.
    const float sea    = m_sim->seaLevelAlongUp();
    const float seaEps = 0.15f; // margen sobre el mar (las celdas fijadas al mar quedan ~en seaLevel)
    auto inlandWet = [&](int i, int j) {
        return m_sim->hasWaterAt(i,j) && m_sim->surfaceAt(i,j) > sea + seaEps;
    };
    m_indices.clear();
    for (int j = 0; j + 1 < n; ++j)
        for (int i = 0; i + 1 < n; ++i) {
            bool wet = inlandWet(i,j) || inlandWet(i+1,j)
                    || inlandWet(i,j+1) || inlandWet(i+1,j+1);
            if (!wet) continue;
            unsigned int a=j*n+i, b=j*n+i+1, c=(j+1)*n+i, d=(j+1)*n+i+1;
            m_indices.insert(m_indices.end(), {a,c,b, b,c,d});
        }
    if (m_indices.empty()) return;

    RHI::Device* dev = RHI::device();
    if (!dev || !RHI::valid(m_pso)) return;
    dev->uploadBuffer(m_vboH, m_verts.size()   * sizeof(float),        m_verts.data());
    dev->uploadBuffer(m_eboH, m_indices.size() * sizeof(unsigned int), m_indices.data());

    // Antes eran uniforms sueltos (locations 15/16); ahora van por UBO (no existen en Vulkan).
    const SoftbodyParams params{ glm::vec3(0.10f, 0.35f, 0.55f), 1.0f };
    dev->updateBuffer(m_uboH, 0, sizeof(params), &params);

    RHI::Context* ctx = dev->beginFrame();
    ctx->bindPipeline(m_pso);            // programa + blend alfa + SIN culling (dos caras)
    ctx->bindVertexBuffer(m_vboH);
    ctx->bindIndexBuffer(m_eboH);
    ctx->bindUniformBuffer(6, m_uboH);
    ctx->drawIndexed((uint32_t)m_indices.size());

    // Transición: el resto del frame sigue en GL directo y espera este estado.
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}

} // namespace Haruka
