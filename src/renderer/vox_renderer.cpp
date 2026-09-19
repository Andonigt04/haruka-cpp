#include "renderer/vox_renderer.h"

#include "core/logger.h"
#include "io/image_writer.h"
#include "renderer/shader.h"
#include "rhi/rhi_context.h"
#include "rhi/rhi_device.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace Haruka {

bool VoxRenderer::ensurePipeline() {
    if (m_failed) return false;
    if (RHI::valid(m_pipe)) return true;
    RHI::Device* dev = RHI::device();
    if (!dev) return false;
    const std::string vs = Shader::baseDir() + "shaders/vox.vert";
    const std::string fs = Shader::baseDir() + "shaders/vox.frag";
    RHI::PipelineDesc pd;
    pd.vertexPath = vs.c_str(); pd.fragmentPath = fs.c_str();
    pd.topology = RHI::PrimitiveTopology::Triangles;
    pd.vertexLayout.strides = { 6 * sizeof(float) };
    pd.vertexLayout.attributes = { { 0, 0, RHI::Format::RGB32F, 0 }, { 1, 3 * sizeof(float), RHI::Format::RGB32F, 0 } };
    pd.depth.test = true; pd.depth.write = true;      // Greater por defecto: reversed-Z del motor
    // `HARUKA_VOX_DRAW=2`: sin test de profundidad (A/B de "no se ve": ¿lo tapa algo o no se dibuja?)
    { const char* e = std::getenv("HARUKA_VOX_DRAW"); if (e && e[0] == '2') { pd.depth.test = false; pd.depth.write = false; } }
    pd.cull = RHI::CullMode::None;                    // se ve desde DENTRO y desde fuera
    m_pipe = dev->createPipeline(pd);
    if (!RHI::valid(m_pipe)) {
        m_failed = true;
        HARUKA_LOGE("Vox", "el pipeline de paredes NO linkó → las cuevas no se dibujan");
        return false;
    }
    VoxUBO init{};
    m_ubo = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(VoxUBO), &init, RHI::BufferMemory::Dynamic);
    return true;
}

void VoxRenderer::update(VoxWorld& world, const glm::dvec3& camRelPlanet, double surfaceElevM) {
    if (!ensurePipeline()) return;
    RHI::Device* dev = RHI::device();
    if (RHI::valid(m_cutTexOld)) { dev->destroy(m_cutTexOld); m_cutTexOld = {}; }

    // ── Mallas: los sucios, acotado por frame ──────────────────────────────────────────────────
    int remeshed = 0;
    std::vector<VoxKey> keys = world.loadedKeys();
    const auto tRemesh0 = std::chrono::high_resolution_clock::now();
    for (const VoxKey& k : keys) {
        VoxChunk* c = world.get(k);
        if (!c || !c->dirty) continue;
        // Presupuesto por TIEMPO, no por número: un chunk todo roca sale en microsegundos y uno
        // con isla en ~2 ms; con "3 por frame" una isla de 20 chunks tardaba 7 s en aparecer (y el
        // jugador que caía sobre ella la atravesaba antes de que existiera).
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tRemesh0).count();
        if (remeshed >= kMaxRemeshPerFrame || (remeshed > 0 && ms > kRemeshBudgetMs)) break;
        CaveMesh mesh;
        world.buildMesh(k, mesh);
        ++remeshed;
        Gpu& g = m_gpu[k];
        if (!RHI::valid(g.ubo)) {
            VoxUBO z{};
            g.ubo = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(VoxUBO), &z, RHI::BufferMemory::Dynamic);
        }
        if (RHI::valid(g.vb)) { dev->destroy(g.vb); g.vb = {}; }
        if (RHI::valid(g.ib)) { dev->destroy(g.ib); g.ib = {}; }
        g.indexCount = 0;
        { CpuMesh& cm = m_cpu[k]; cm.mesh = mesh; ++cm.revision; }
        if (mesh.empty()) continue;   // todo roca o todo aire: nada que dibujar, y no cuesta
        std::vector<float> vtx; vtx.reserve(mesh.positions.size() * 6);
        for (size_t i = 0; i < mesh.positions.size(); ++i) {
            const glm::vec3& p = mesh.positions[i]; const glm::vec3& n = mesh.normals[i];
            vtx.insert(vtx.end(), { p.x, p.y, p.z, n.x, n.y, n.z });
        }
        g.vb = dev->createBuffer(RHI::BufferUsage::Vertex, vtx.size() * sizeof(float), vtx.data(), RHI::BufferMemory::Static);
        g.ib = dev->createBuffer(RHI::BufferUsage::Index, mesh.indices.size() * sizeof(unsigned), mesh.indices.data(), RHI::BufferMemory::Static);
        g.indexCount = (uint32_t)mesh.indices.size();
        g.origin = mesh.origin;
        HARUKA_LOGI("Vox", "malla chunk (%d,%d,%d,%d): %zu vertices · %zu triangulos",
                    k.face, k.i, k.j, k.k, mesh.positions.size(), mesh.triangleCount());
        // `HARUKA_VOX_DUMP=<carpeta>`: cada malla a un OBJ (posiciones relativas al origen del
        // chunk, con normales) para mirarla fuera del juego cuando lo que se ve no cuadra con lo
        // que dicen los tests.
        static const char* s_dump = std::getenv("HARUKA_VOX_DUMP");
        if (s_dump && s_dump[0]) {
            char name[256];
            std::snprintf(name, sizeof(name), "%s/vox_%d_%d_%d_%d.obj", s_dump, k.face, k.i, k.j, k.k);
            if (std::FILE* f = std::fopen(name, "w")) {
                std::fprintf(f, "# origen %.3f %.3f %.3f\n", mesh.origin.x, mesh.origin.y, mesh.origin.z);
                for (const glm::vec3& p : mesh.positions) std::fprintf(f, "v %.4f %.4f %.4f\n", p.x, p.y, p.z);
                for (const glm::vec3& n : mesh.normals) std::fprintf(f, "vn %.4f %.4f %.4f\n", n.x, n.y, n.z);
                for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
                    std::fprintf(f, "f %u//%u %u//%u %u//%u\n", mesh.indices[i] + 1, mesh.indices[i] + 1,
                                 mesh.indices[i + 1] + 1, mesh.indices[i + 1] + 1, mesh.indices[i + 2] + 1, mesh.indices[i + 2] + 1);
                std::fclose(f);
            }
        }
    }
    // Lo descargado se suelta de la GPU.
    for (auto it = m_gpu.begin(); it != m_gpu.end();) {
        if (world.get(it->first)) { ++it; continue; }
        if (RHI::valid(it->second.vb)) dev->destroy(it->second.vb);
        if (RHI::valid(it->second.ib)) dev->destroy(it->second.ib);
        if (RHI::valid(it->second.ubo)) dev->destroy(it->second.ubo);
        m_cpu.erase(it->first);
        it = m_gpu.erase(it);
    }

    // ── La ventana de recorte: si el campo cambió o el pie se movió más de 40 m ────────────────
    const double camR = glm::length(camRelPlanet);
    if (camR < 1.0) return;
    const glm::dvec3 foot = camRelPlanet / camR * (world.planetRadius() + surfaceElevM);
    if (world.version() != m_cutVersion || remeshed > 0 || glm::length(foot - m_cutCenter) > 40.0)
        rebuildCut(world, camRelPlanet, surfaceElevM);
    // ⚠️ LA MATRIZ SE REHACE CADA FRAME, LA TEXTURA NO. `vFragPos` es relativo a la cámara de ESTE
    // frame; la textura se rehace cada 40 m. Con la matriz congelada en el rehecho, la ventana se
    // movía con la cámara hasta 40 m antes de volver a su sitio: el agujero se desplazaba al andar.
    // El marco (e1, e2, up, pie) es el del rehecho; sólo la traslación cambia.
    else refreshCutSpace(camRelPlanet);
}

void VoxRenderer::rebuildCut(VoxWorld& world, const glm::dvec3& camRelPlanet, double surfaceElevM) {
    RHI::Device* dev = RHI::device();
    const double camR = glm::length(camRelPlanet);
    const glm::dvec3 up = camRelPlanet / camR;
    const glm::dvec3 foot = up * (world.planetRadius() + surfaceElevM);
    const glm::dvec3 ref = (std::fabs(up.y) < 0.9) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const glm::dvec3 e1 = glm::normalize(glm::cross(ref, up)), e2 = glm::cross(up, e1);

    m_cutPixels.assign((size_t)kCutRes * kCutRes * 4, 0);
    size_t abiertos = 0;
    for (int y = 0; y < kCutRes; ++y)
        for (int x = 0; x < kCutRes; ++x) {
            const double fx = ((double)x / (kCutRes - 1) - 0.5) * 2.0 * kCutHalfM;
            const double fy = ((double)y / (kCutRes - 1) - 0.5) * 2.0 * kCutHalfM;
            const glm::dvec3 dir = glm::normalize(foot + e1 * fx + e2 * fy);
            // ⚠️ SÓLO DONDE YA HAY PARED QUE ENSEÑAR. Si el chunk de la superficie está sin remallar
            // (3 por frame), recortar el suelo deja un agujero al vacío durante unos frames — y por él
            // se ve el plano del mar/lago, o el cielo. El recorte espera a la malla.
            // ⚠️ "MALLADO ALGUNA VEZ", NO "NO SUCIO". Con `!dirty`, cada trazo (que ensucia el chunk y
            // a sus 26 vecinos) apagaba el recorte hasta que el remallado los alcanzaba: el suelo
            // original reaparecía un momento sobre el agujero ("la malla original fantasma").
            // Mientras se remalla, la malla vieja sigue dibujándose, así que el recorte puede quedarse.
            const VoxKey sk = world.keyAt(dir * (world.planetRadius() + (double)(world.elevAt(dir)) - 3.0));
            const VoxChunk* sc = world.get(sk);
            const float cut = (sc && m_cpu.count(sk)) ? world.surfaceCut(dir) : 0.0f;
            const unsigned char v = (unsigned char)std::lround(cut * 255.0f);
            // G = el LECHO: metros de aire bajo la superficie (hasta 255). Es lo que baja el suelo
            // para el agua: la lámina de un lago que cubra la boca se dibuja DENTRO del foso, y la
            // que quede por debajo del lecho, no. Sólo se calcula donde hay recorte (baja por la
            // columna metro a metro).
            const float depth = (v > 0) ? world.floorDepthM(dir, 255.0f) : 0.0f;
            const size_t o = ((size_t)y * kCutRes + x) * 4;
            m_cutPixels[o + 0] = v;
            m_cutPixels[o + 1] = (unsigned char)std::lround(std::min(depth, 255.0f));
            m_cutPixels[o + 2] = 0; m_cutPixels[o + 3] = 255;
            if (v > 127) ++abiertos;
        }
    // El RHI no tiene "actualizar textura": se recrea, como hace la ventana de lagos del planeta.
    // ⚠️ La textura vieja NO se destruye en este frame: puede estar atada a un command buffer en
    // vuelo (Vulkan). Se aparca y se suelta en el siguiente `update`.
    if (RHI::valid(m_cutTex)) m_cutTexOld = m_cutTex;
    RHI::TextureDesc td; td.width = kCutRes; td.height = kCutRes; td.format = RHI::Format::RGBA8;   // R = recorte · G = lecho (m)
    td.filter = RHI::Filter::Linear; td.wrap = RHI::Wrap::ClampToEdge; td.mipmaps = false;
    td.initialData = m_cutPixels.data();
    m_cutTex = dev->createTexture(td);
    m_cutCenter = foot; m_cutE1 = e1; m_cutE2 = e2; m_cutUp = up;
    refreshCutSpace(camRelPlanet);
    const glm::dvec3 footRelCam = foot - camRelPlanet;
    m_cutVersion = world.version();
    if (const char* dump = std::getenv("HARUKA_VOX_CUT_PNG")) {   // lo que la CPU cree que sube
        writePNG(dump, kCutRes, kCutRes, 4, m_cutPixels.data());
    }
    m_cutReady = true;
    static uint64_t s_log = 0;
    if (abiertos > 0 && (s_log++ % 30) == 0) {
        // Y el valor BAJO LOS PIES: si la cámara está sobre una boca y aquí sale 0, el recorte no
        // puede verse por mucho que la ventana tenga téxeles abiertos en otra parte.
        const unsigned char pie = m_cutPixels[((size_t)(kCutRes / 2) * kCutRes + kCutRes / 2) * 4];
        // Comprobación de la matriz con el propio pie: tiene que caer en (0,5, 0,5).
        const glm::vec4 uvPie = m_cutSpace * glm::vec4(glm::vec3(footRelCam), 1.0f);
        const glm::vec4 uv100 = m_cutSpace * glm::vec4(glm::vec3(footRelCam + e1 * 100.0), 1.0f);
        HARUKA_LOGI("Vox", "  matriz: pie+100m·e1 -> uv (%.3f, %.3f) (esperado 0,656, 0,5)", uv100.x, uv100.y);
        HARUKA_LOGI("Vox", "ventana de recorte: %zu texeles abiertos de %d (%.2f %%) · %zu chunks · bajo los pies %u/255 · uv(pie)=(%.3f, %.3f)",
                    abiertos, kCutRes * kCutRes, 100.0 * abiertos / (kCutRes * kCutRes), world.loadedCount(),
                    (unsigned)pie, uvPie.x, uvPie.y);
    }
}

void VoxRenderer::refreshCutSpace(const glm::dvec3& camRelPlanet) {
    // De posición RELATIVA A LA CÁMARA a uv de la ventana: primero al marco (e1, e2) centrado en
    // el pie, luego a [0,1]. El pie relativo a la cámara es (foot − cam), con la cámara de AHORA.
    const glm::dvec3 footRelCam = m_cutCenter - camRelPlanet;
    const glm::dvec3 &e1 = m_cutE1, &e2 = m_cutE2, &up = m_cutUp;
    glm::dmat4 toFrame(1.0);
    toFrame[0] = glm::dvec4(e1.x, e2.x, up.x, 0.0);
    toFrame[1] = glm::dvec4(e1.y, e2.y, up.y, 0.0);
    toFrame[2] = glm::dvec4(e1.z, e2.z, up.z, 0.0);
    toFrame[3] = glm::dvec4(-glm::dot(footRelCam, e1), -glm::dot(footRelCam, e2), -glm::dot(footRelCam, up), 1.0);
    glm::dmat4 toUV(1.0);
    toUV[0][0] = 0.5 / kCutHalfM; toUV[1][1] = 0.5 / kCutHalfM; toUV[3] = glm::dvec4(0.5, 0.5, 0.0, 1.0);
    m_cutSpace = glm::mat4(toUV * toFrame);
}

void VoxRenderer::draw(const VoxWorld& world, const glm::mat4& rotVP, const glm::dvec3& camRelPlanet,
                       const glm::vec3& sunDir, const glm::vec3& viewDir) {
    m_drawn = 0; m_tris = 0; m_culled = 0;
    static const bool s_off = [] { const char* e = std::getenv("HARUKA_VOX_DRAW"); return e && e[0] == '0'; }();
    if (s_off || !RHI::valid(m_pipe) || m_gpu.empty()) return;
    RHI::Device* dev = RHI::device();
    RHI::Context* ctx = dev->beginFrame();
    if (!ctx) return;
    // FRUSTUM: los seis planos salen de `rotVP` (Gribb-Hartmann) en el espacio relativo al ojo, que
    // es el de las posiciones. Reversed-Z con lejano infinito: el plano "lejano" es degenerado y no
    // se usa. Cada chunk se prueba como ESFERA (centro + radio de su diagonal/2): conservador, nunca
    // descarta algo visible.
    glm::vec4 planes[5];
    {
        const glm::mat4 m = glm::transpose(rotVP);   // filas de rotVP
        planes[0] = m[3] + m[0];   // izquierda
        planes[1] = m[3] - m[0];   // derecha
        planes[2] = m[3] + m[1];   // abajo
        planes[3] = m[3] - m[1];   // arriba
        planes[4] = m[2];          // cercano (reversed-Z: z_clip >= 0)
        for (auto& p : planes) { const float l = glm::length(glm::vec3(p)); if (l > 1e-9f) p /= l; }
    }
    (void)viewDir;
    ctx->bindPipeline(m_pipe);
    for (const auto& [key, g] : m_gpu) {
        if (!g.indexCount || !RHI::valid(g.vb)) continue;
        const VoxChunk* c = world.get(key);
        if (!c) continue;
        {
            const glm::vec3 centerRel = glm::vec3(c->centerDir * (c->baseR + 0.5 * kVoxChunkM) - camRelPlanet);
            // ⚠️ ×1,3 de margen: el chunk no es una esfera ni un cubo (es un trozo de casquete), y con
            // el radio justo la contraprueba de abajo cazó descartes con una esquina en pantalla.
            const float radius = 1.3f * 0.5f * std::sqrt(c->sideU * c->sideU + c->sideV * c->sideV + (float)(kVoxChunkM * kVoxChunkM)) + 4.0f;
            static const bool s_noCull = [] { const char* e = std::getenv("HARUKA_VOX_NOCULL"); return e && e[0] == '1'; }();
            bool out = false;
            for (const auto& p : planes)
                if (s_noCull) break; else
                if (glm::dot(glm::vec3(p), centerRel) + p.w < -radius) { out = true; break; }
            if (out) {
                // CONTRAPRUEBA del cull: si alguna de las 8 esquinas del chunk cae DENTRO del clip
                // (todas las coordenadas |x|,|y| <= w con w > 0), el descarte es falso y se dice.
                bool esquinaDentro = false;
                for (int cz = 0; cz <= 1 && !esquinaDentro; ++cz)
                    for (int cy = 0; cy <= 1 && !esquinaDentro; ++cy)
                        for (int cx = 0; cx <= 1 && !esquinaDentro; ++cx) {
                            const double N = (double)(kVoxN - 1);
                            const glm::vec3 rel = glm::vec3(c->worldOfGrid(cx * N, cy * N, cz * N) - camRelPlanet);
                            const glm::vec4 q = rotVP * glm::vec4(rel, 1.0f);
                            if (q.w > 0.0f && std::fabs(q.x) <= q.w && std::fabs(q.y) <= q.w) esquinaDentro = true;
                        }
                static int s_falsos = 0;
                if (esquinaDentro && (s_falsos++ % 60) == 0)
                    HARUKA_LOGW("Vox", "CULL FALSO: chunk (%d,%d,%d,%d) descartado con una esquina en pantalla",
                                key.face, key.i, key.j, key.k);
                ++m_culled; continue;
            }
        }
        VoxUBO u{};
        u.rotVP = rotVP;
        u.originRel = glm::vec4(glm::vec3(g.origin - camRelPlanet), 0.0f);
        u.lightDir = glm::vec4(sunDir, 0.0f);
        u.color = glm::vec4(0.42f, 0.38f, 0.34f, 1.0f);
        u.up    = glm::vec4(glm::vec3(c->centerDir), 0.0f);
        // Origen del chunk reducido módulo el tile EN DOUBLES: lo que llega al shader es pequeño y
        // exacto, y como el origen es fijo en el mundo la textura no se mueve con la cámara.
        const double T = (double)std::max(m_tiling, 0.01f);
        const glm::dvec3 o = g.origin;
        u.texOrigin = glm::vec4((float)(o.x - std::floor(o.x / T) * T), (float)(o.y - std::floor(o.y / T) * T),
                                (float)(o.z - std::floor(o.z / T) * T), RHI::valid(m_albedo) ? (float)m_rockLayer : -1.0f);
        u.texInfo = glm::vec4((float)T, 0.0f, 0.0f, 0.0f);
        // ⚠️ UN UBO POR CHUNK. `updateBuffer` sobre el mismo buffer entre dos draws del mismo
        // command buffer NO se versiona: todos los draws leerían la última escritura (el pase de
        // nodos, por eso, sube su UBO UNA vez antes de cualquier draw).
        dev->updateBuffer(g.ubo, 0, sizeof(u), &u);
        ctx->bindUniformBuffer(5, g.ubo);
        // Siempre atado (descriptor indefinido en Vulkan = pérdida del dispositivo): sin array de
        // roca se ata el mismo dummy del recorte, y `texOrigin.w < 0` hace que no se lea.
        ctx->bindTexture(1, RHI::valid(m_albedo) ? m_albedo : m_cutTex);
        ctx->bindVertexBuffer(g.vb);
        ctx->bindIndexBuffer(g.ib);
        ctx->drawIndexed(g.indexCount, 0, 1);
        ++m_drawn; m_tris += g.indexCount / 3;
    }
    static bool s_first = true;
    static auto s_last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if ((m_drawn > 0 && s_first) || std::chrono::duration<double>(now - s_last).count() > 5.0) {
        s_first = m_drawn == 0; s_last = now;
        size_t sucios = 0, sinMalla = 0;
        for (const VoxKey& k : world.loadedKeys()) {
            const VoxChunk* c = world.get(k);
            if (c && c->dirty) ++sucios;
            if (!m_gpu.count(k)) ++sinMalla;
        }
        int caras = 0; const float costura = world.seamMismatch(&caras);
        HARUKA_LOGI("Vox", "dibujados %zu chunks · %zu triangulos · fuera del frustum %zu · cargados %zu (con contenido %zu · %.1f MB de voxel · sucios %zu, sin malla %zu) · costura peor %.3f m en %d caras",
                    m_drawn, m_tris, m_culled, world.loadedCount(), world.loadedWithContent(), world.voxelBytes() / 1048576.0, sucios, sinMalla, costura, caras);
    }
}

} // namespace Haruka
