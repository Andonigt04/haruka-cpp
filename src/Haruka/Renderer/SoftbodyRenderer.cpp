#include "SoftbodyRenderer.h"
#include "Haruka/RHI/RHIDevice.h"
#include "Haruka/RHI/RHIContext.h"
#include "Haruka/Renderer/Shader.h"
#include "Haruka/Core/AssetPaths.h"
#include "Haruka/Core/Logger.h"
#include <glm/glm.hpp>
#include <stb_image.h>

namespace Haruka::Renderer {

SoftBodyRenderer::~SoftBodyRenderer() { clear(); }

void SoftBodyRenderer::clear() {
    RHI::Device* dev = RHI::device();
    for (auto& e : m_entries) {
        if (dev) {
            if (RHI::valid(e.hVbo)) dev->destroy(e.hVbo);
            if (RHI::valid(e.hEbo)) dev->destroy(e.hEbo);
            if (RHI::valid(e.hParams)) dev->destroy(e.hParams);
            if (RHI::valid(e.tex) && e.tex.id != m_white.id) dev->destroy(e.tex);
        }
    }
    m_entries.clear();
    if (dev) {
        if (RHI::valid(m_psoMesh))   { dev->destroy(m_psoMesh);   m_psoMesh   = {}; }
        if (RHI::valid(m_white))     { dev->destroy(m_white);     m_white     = {}; }
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
            if (RHI::valid(it->tex) && it->tex.id != m_white.id) dev->destroy(it->tex);
        }
        m_entries.erase(it);
        return;
    }
}

void SoftBodyRenderer::uploadEntry(Entry& e, const Tools::WorldPos& cameraPos) {
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

void SoftBodyRenderer::render(const Tools::WorldPos& cameraPos) {
    if (m_entries.empty()) return;
    ensurePSO();
    if (!RHI::valid(m_pso) || !RHI::valid(m_paramsUBO)) return;

    RHI::Device* dev = RHI::device();
    if (!dev) return;
    RHI::Context* ctx = dev->beginFrame();

    ctx->bindPipeline(m_pso);

    for (auto& e : m_entries) {
        if (!e.initialized || !e.solver || e.mesh) continue;
        uploadEntry(e, cameraPos);
        ctx->bindUniformBuffer(6, e.hParams);

        ctx->bindVertexBuffer(e.hVbo, 0);
        ctx->bindIndexBuffer(e.hEbo);
        ctx->drawIndexed((uint32_t)e.handle.renderIndices.size());
    }

    // ── las MALLAS DE AUTOR (addMesh): su propio pipeline, con uv, color y textura ──
    bool anyMesh = false;
    for (const auto& e : m_entries) anyMesh |= (e.mesh && e.initialized && e.solver);
    if (!anyMesh) return;
    ensureMeshPSO();
    if (!RHI::valid(m_psoMesh)) return;
    ctx->bindPipeline(m_psoMesh);
    for (auto& e : m_entries) {
        if (!e.mesh || !e.initialized || !e.solver) continue;
        uploadMesh(e, cameraPos);
        ctx->bindUniformBuffer(6, e.hParams);
        ctx->bindTexture(1, RHI::valid(e.tex) ? e.tex : m_white);
        ctx->bindVertexBuffer(e.hVbo, 0);
        ctx->bindIndexBuffer(e.hEbo);
        ctx->drawIndexed((uint32_t)e.md.indices.size());
    }
}

void SoftBodyRenderer::ensureMeshPSO() {
    if (RHI::valid(m_psoMesh)) return;
    RHI::Device* dev = RHI::device();
    if (!dev) return;
    const std::string vs = Shader::baseDir() + "shaders/softbody_mesh.vert";
    const std::string fs = Shader::baseDir() + "shaders/softbody_mesh.frag";
    RHI::PipelineDesc pd;
    pd.vertexPath   = vs.c_str();
    pd.fragmentPath = fs.c_str();
    pd.cull         = RHI::CullMode::None;   // dos caras, como la manta: una piel se ve desde dentro al abrirse
    pd.depth.test   = true;
    pd.depth.write  = true;                  // reversed-Z: sin `compare` (ver ensurePSO)
    pd.topology     = RHI::PrimitiveTopology::Triangles;
    pd.vertexLayout.strides = { 11 * sizeof(float) };
    pd.vertexLayout.attributes = {
        { 0, 0,                 RHI::Format::RGB32F, 0 },   // aPos (relativa a la cámara)
        { 1, 3 * sizeof(float), RHI::Format::RGB32F, 0 },   // aNormal
        { 2, 6 * sizeof(float), RHI::Format::RG32F,  0 },   // aUV
        { 3, 8 * sizeof(float), RHI::Format::RGB32F, 0 },   // aColor
    };
    m_psoMesh = dev->createPipeline(pd);
    if (!RHI::valid(m_white)) {
        const uint8_t px[4] = { 255, 255, 255, 255 };
        RHI::TextureDesc td; td.width = 1; td.height = 1; td.format = RHI::Format::RGBA8;
        td.filter = RHI::Filter::Nearest; td.wrap = RHI::Wrap::ClampToEdge; td.mipmaps = false; td.initialData = px;
        m_white = dev->createTexture(td);
    }
}

int SoftBodyRenderer::addMesh(const MeshDraw& md, Physics::XPBD::XPBDSolver* solver) {
    RHI::Device* dev = RHI::device();
    if (!dev || md.indices.empty() || md.vertexParticle.empty()) return 0;
    Entry e;
    e.id = m_nextId++;
    e.solver = solver;
    e.mesh = true;
    e.md = md;
    e.color = md.tint;
    e.hEbo = dev->createBuffer(RHI::BufferUsage::Index, md.indices.size() * sizeof(uint32_t), md.indices.data());
    e.hVbo = dev->createBuffer(RHI::BufferUsage::Vertex, 0, nullptr, RHI::BufferMemory::Stream);
    // Textura: la ruta llega del JSON (relativa al proyecto); se prueban las mismas raíces que los materiales.
    bool hasTex = false;
    if (!md.texture.empty()) {
        const std::string cands[] = { md.texture, Core::AssetPaths::projectRoot() + md.texture,
                                      Core::AssetPaths::assetsDir() + md.texture };
        int w = 0, h = 0, n = 0; unsigned char* px = nullptr;
        for (const auto& c : cands) { if (!c.empty() && (px = stbi_load(c.c_str(), &w, &h, &n, 4))) break; }
        if (px) {
            RHI::TextureDesc td; td.width = w; td.height = h; td.format = RHI::Format::RGBA8;
            td.filter = RHI::Filter::Linear; td.wrap = RHI::Wrap::Repeat; td.mipmaps = true; td.initialData = px;
            e.tex = dev->createTexture(td);
            stbi_image_free(px);
            hasTex = RHI::valid(e.tex);
        } else {
            HARUKA_LOGW("SoftBody", "textura de la piel no encontrada: %s (se dibuja con su color)", md.texture.c_str());
        }
    }
    // vec3 tint + float hasTexture (std140, 16 B): `softbody_mesh.frag`
    const float params[4] = { md.tint.x, md.tint.y, md.tint.z, hasTex ? 1.0f : 0.0f };
    e.hParams = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(params), params, RHI::BufferMemory::Dynamic);
    e.initialized = true;
    const int id = e.id;
    m_entries.push_back(std::move(e));
    return id;
}

void SoftBodyRenderer::uploadMesh(Entry& e, const Tools::WorldPos& cameraPos) {
    const auto& ps = e.solver->particles;
    const auto& md = e.md;
    const int np = ps.count();
    // Normales por PARTÍCULA (no por vértice de dibujo): así una costura de UV no parte el sombreado.
    std::vector<glm::vec3> pos((size_t)np), nrm((size_t)np, glm::vec3(0.0f));
    std::vector<char> used((size_t)np, 0);
    for (int p : md.vertexParticle) if (p >= 0 && p < np) used[(size_t)p] = 1;
    for (int p = 0; p < np; ++p) if (used[(size_t)p]) pos[(size_t)p] = glm::vec3(ps.worldPos(p) - glm::dvec3(cameraPos));
    for (size_t t = 0; t + 2 < md.indices.size(); t += 3) {
        const int a = md.vertexParticle[md.indices[t]], b = md.vertexParticle[md.indices[t + 1]],
                  c = md.vertexParticle[md.indices[t + 2]];
        if (a < 0 || b < 0 || c < 0 || a >= np || b >= np || c >= np) continue;
        const glm::vec3 fn = glm::cross(pos[(size_t)b] - pos[(size_t)a], pos[(size_t)c] - pos[(size_t)a]);
        nrm[(size_t)a] += fn; nrm[(size_t)b] += fn; nrm[(size_t)c] += fn;
    }
    const size_t nv = md.vertexParticle.size();
    e.cpuVerts.resize(nv * 11);
    for (size_t v = 0; v < nv; ++v) {
        const int p = md.vertexParticle[v];
        const bool ok = p >= 0 && p < np;
        const glm::vec3 x = ok ? pos[(size_t)p] : glm::vec3(0.0f);
        glm::vec3 n = ok ? nrm[(size_t)p] : glm::vec3(0, 1, 0);
        const float l = glm::length(n);
        n = l > 1e-8f ? n / l : glm::vec3(0, 1, 0);
        const glm::vec2 uv = v < md.uv.size() ? md.uv[v] : glm::vec2(0.0f);
        const glm::vec3 c  = v < md.color.size() ? md.color[v] : glm::vec3(1.0f);
        float* o = &e.cpuVerts[v * 11];
        o[0] = x.x; o[1] = x.y; o[2] = x.z; o[3] = n.x; o[4] = n.y; o[5] = n.z;
        o[6] = uv.x; o[7] = uv.y; o[8] = c.x; o[9] = c.y; o[10] = c.z;
    }
    if (RHI::Device* dev = RHI::device())
        dev->uploadBuffer(e.hVbo, e.cpuVerts.size() * sizeof(float), e.cpuVerts.data());
}

} // namespace Haruka::Renderer