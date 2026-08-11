#include "shallow_water_renderer.h"
#include "shader.h"
#include "rhi/rhi_context.h"

namespace {
// std140 (binding 6): dos bloques vec3(12)+float(4) = 2 x 16 B = 32 B.
struct ShallowWaterParams { glm::vec3 color; float alphaMode; glm::vec3 foamColor; float foamStrength; };
static_assert(sizeof(ShallowWaterParams) == 32, "ShallowWaterParams std140 size mismatch");
}
#include "physics/fluid/shallow_water.h"
#include "rhi/rhi_device.h"
#include "core/logger.h"
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
    const std::string vs = Shader::baseDir() + "shaders/shallow_water.vert";
    const std::string fs = Shader::baseDir() + "shaders/shallow_water.frag";
    RHI::PipelineDesc pd;
    pd.vertexPath          = vs.c_str();
    pd.fragmentPath        = fs.c_str();
    pd.vertexLayout.strides = { (uint32_t)(8 * sizeof(float)) };   // pos(3)+normal(3)+alpha(1)+foam(1)
    pd.vertexLayout.attributes = {
        { 0, 0,                 RHI::Format::RGB32F },
        { 1, 3 * sizeof(float), RHI::Format::RGB32F },
        { 2, 6 * sizeof(float), RHI::Format::R32F   },   // alpha por-vértice (fade de borde)
        { 3, 7 * sizeof(float), RHI::Format::R32F   },   // foam (espuma de cresta rompiente)
    };
    pd.topology     = RHI::PrimitiveTopology::Triangles;
    pd.depth.test   = true;  pd.depth.write = true;
    pd.blend.enable = true;  pd.blend.mode  = RHI::BlendMode::Alpha;
    pd.cull         = RHI::CullMode::None;        // agua a DOS CARAS (antes: glDisable(GL_CULL_FACE))
    m_pso  = dev->createPipeline(pd);
    m_vboH = dev->createBuffer(RHI::BufferUsage::Vertex,  0, nullptr, RHI::BufferMemory::Stream);
    m_eboH = dev->createBuffer(RHI::BufferUsage::Index,   0, nullptr, RHI::BufferMemory::Stream);
    m_uboH = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(ShallowWaterParams), nullptr,
                               RHI::BufferMemory::Dynamic);
    m_init = true;
}

void ShallowWaterRenderer::render(const Haruka::WorldPos& cameraPos) {
    if (!m_sim) return;
    ensureGL();

    // Guarda anti-crash: si el grid no está dimensionado (arrays CPU vacíos) los accessors
    // m_terrain[i]/m_water[i] leen fuera del vector → SIGSEGV (suele manifestarse como crash en
    // el primer clamp/min del building de vértices). Log del valor real para diagnosticar.
    const int n = m_sim->size();
    if (!m_sim->valid() || n > 4096)
    {
        HARUKA_LOGE("Fluid", "ShallowWater sim inválido (grid n=%d) — agua saltada", n);
        return;
    }
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

    // ── ÍNDICES PRIMERO. ────────────────────────────────────────────────────────────────────────
    //
    // ⚠️ Esto estaba AL FINAL, y era el derroche gordo: se construían los 9216 vértices con sus
    // normales, su alpha y su espuma, y DESPUÉS se miraba si alguna celda tenía agua. Sin agua a la
    // vista —el caso normal— todo ese trabajo se tiraba: 4,95 ms por frame para no dibujar nada.
    // Decidir la topología primero cuesta dos lecturas por celda y permite salir antes de tocar un
    // solo vértice.
    //
    // Triángulos solo donde algún vértice del quad tiene agua POR ENCIMA del mar: las celdas a/bajo
    // el nivel del mar las dibujan los chunks de océano (Gerstner) → aquí se saltan para que no haya
    // DOBLE lámina en la costa. Sin océano acoplado (seaLevel=-1e9) se dibuja todo como antes.
    const float sea    = m_sim->seaLevelAlongUp();
    const float seaEps = 0.15f; // margen sobre el mar (las celdas fijadas al mar quedan ~en seaLevel)
    auto inlandWet = [&](int i, int j) {
        return m_sim->hasWaterAt(i,j) && m_sim->surfaceAt(i,j) > sea + seaEps;
    };
    m_indices.clear();   // conserva la capacidad entre frames: no re-asigna
    for (int j = 0; j + 1 < n; ++j)
        for (int i = 0; i + 1 < n; ++i) {
            bool wet = inlandWet(i,j) || inlandWet(i+1,j)
                    || inlandWet(i,j+1) || inlandWet(i+1,j+1);
            if (!wet) continue;
            const unsigned int a=j*n+i, b=j*n+i+1, c=(j+1)*n+i, d=(j+1)*n+i+1;
            m_indices.push_back(a); m_indices.push_back(c); m_indices.push_back(b);
            m_indices.push_back(b); m_indices.push_back(c); m_indices.push_back(d);
        }
    if (m_indices.empty()) return;   // ← la salida que faltaba: sin agua, ni un vértice

    // Rejilla de vértices. `m_pos` y `m_verts` son MIEMBROS reutilizados: antes `pos` se asignaba
    // en el heap cada frame (9216 elementos) y `m_verts` se hacía clear+resize, o sea 73 728 floats
    // puestos a cero para sobrescribirlos justo después.
    const size_t vcount = size_t(n) * n;
    if (m_pos.size()   != vcount)     m_pos.resize(vcount);
    if (m_verts.size() != vcount * 8) m_verts.resize(vcount * 8);
    auto& pos = m_pos;
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
            // Breaking-crest foam: where the surface tilts strongly from `up` the
            // wavefront is steep (curling/breaking). Scale by water depth so dry,
            // slope-only terrain does not foam. `smoothstep` gives a crisp-ish line
            // on the surf while the depth fade keeps the interior calm.
            float tilt = 1.0f - glm::dot(nrm, glm::vec3(up));
            float foam = glm::smoothstep(0.22f, 0.55f, tilt)
                       * glm::smoothstep(0.05f, 0.50f, depth);
            int b = (j*n+i)*8;
            m_verts[b+0]=pos[j*n+i].x; m_verts[b+1]=pos[j*n+i].y; m_verts[b+2]=pos[j*n+i].z;
            m_verts[b+3]=nrm.x; m_verts[b+4]=nrm.y; m_verts[b+5]=nrm.z;
            m_verts[b+6]=alpha;
            m_verts[b+7]=foam;
        }

    RHI::Device* dev = RHI::device();
    if (!dev || !RHI::valid(m_pso)) return;
    dev->uploadBuffer(m_vboH, m_verts.size()   * sizeof(float),        m_verts.data());
    dev->uploadBuffer(m_eboH, m_indices.size() * sizeof(unsigned int), m_indices.data());

    // Antes eran uniforms sueltos (locations 15/16); ahora van por UBO (no existen en Vulkan).
    const ShallowWaterParams params{
        glm::vec3(0.10f, 0.35f, 0.55f),   // water colour
        1.0f,                             // per-vertex alpha
        glm::vec3(0.90f, 0.97f, 1.0f),    // foam colour (near-white, hint of cyan)
        1.2f,                             // foam strength (can exceed 1 → stronger mix)
    };
    dev->updateBuffer(m_uboH, 0, sizeof(params), &params);

    RHI::Context* ctx = dev->beginFrame();
    ctx->bindPipeline(m_pso);            // programa + blend alfa + SIN culling (dos caras)
    ctx->bindVertexBuffer(m_vboH);
    ctx->bindIndexBuffer(m_eboH);
    ctx->bindUniformBuffer(6, m_uboH);
    ctx->drawIndexed((uint32_t)m_indices.size());
}

} // namespace Haruka
