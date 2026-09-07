// Haruka — miniatura 3D compartida por el juego y el editor. Ver tools/preview_render.h.
#include "tools/preview_render.h"

#include "core/application.h"
#include "renderer/model.h"
#include "renderer/motor_instance.h"
#include "renderer/shader.h"             // Shader::baseDir(): raíz de assets, igual que el motor
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"

#include <glm/gtc/matrix_transform.hpp>

#include <unordered_map>
#include <cmath>
#include <SDL3/SDL_log.h>

namespace Haruka {
namespace {

namespace R = Haruka::RHI;

using Haruka::Core::Application;
using Haruka::Renderer::MotorInstance;

/// Todo lo que vive por miniatura. El UBO es POR DIBUJO a propósito: ver la nota en previewTexture().
struct Cached {
    R::RenderPassHandle rt{};
    R::TextureHandle    tex{};
    R::BufferHandle     vb{}, ib{}, ubo{};
    bool                failed = false;    // no se pudo construir: no reintentar cada frame
};
std::unordered_map<std::string, Cached> g_cache;

R::PipelineHandle g_pipe{};
bool              g_pipeFailed = false;

/// Lo que ve el shader en `binding = 0`. Debe casar EXACTAMENTE con ItemPreviewUBO del .vert.
struct PreviewUBO { glm::mat4 mvp; glm::mat4 model; };

R::PipelineHandle pipeline() {
    if (g_pipe.id || g_pipeFailed) return g_pipe;
    R::Device* dev = R::device();
    if (!dev) return {};

    const std::string vs = Haruka::Shader::baseDir() + "shaders/item_preview.vert";
    const std::string fs = Haruka::Shader::baseDir() + "shaders/item_preview.frag";

    R::PipelineDesc pd;
    pd.vertexPath   = vs.c_str();
    pd.fragmentPath = fs.c_str();
    pd.vertexLayout.strides = { 9 * sizeof(float) };          // pos3 + nrm3 + col3 interleaved
    pd.vertexLayout.attributes = {
        { 0, 0,                 R::Format::RGB32F, 0 },
        { 1, 3 * sizeof(float), R::Format::RGB32F, 0 },
        { 2, 6 * sizeof(float), R::Format::RGB32F, 0 },
    };
    // El icono NO usa reversed-Z: se proyecta con una ortográfica propia y su profundidad se limpia
    // a 1. Se dice aquí explícitamente porque el default del motor es Greater (reversed-Z) y heredarlo
    // dejaría el icono vacío — que fue exactamente el fallo mudo de la versión en GL crudo.
    pd.depth.test = true;
    pd.depth.write = true;
    pd.depth.compare = R::CompareOp::Less;
    pd.blend.enable = false;
    pd.cull = R::CullMode::None;

    g_pipe = dev->createPipeline(pd);
    if (!g_pipe.id) {
        SDL_Log("[ItemPreview] el pipeline no se pudo crear -> la UI usa iconos 2D");
        g_pipeFailed = true;
    }
    return g_pipe;
}

} // namespace

void previewAddMesh(PreviewGeo& g, const std::vector<glm::vec3>& pos,
                    const std::vector<glm::vec3>& nrm, const std::vector<glm::vec3>& col,
                    const std::vector<unsigned int>& idx) {
    const unsigned base = (unsigned)(g.data.size() / 9);
    for (size_t i = 0; i < pos.size(); ++i) {
        const glm::vec3 n = i < nrm.size() ? nrm[i] : glm::vec3(0,1,0);
        const glm::vec3 c = i < col.size() ? col[i] : glm::vec3(0.8f);
        for (float x : {pos[i].x,pos[i].y,pos[i].z, n.x,n.y,n.z, c.x,c.y,c.z}) g.data.push_back(x);
    }
    for (unsigned i : idx) g.idx.push_back(base + i);
}

void previewAddBox(PreviewGeo& g, const glm::vec3& half, const glm::vec3& col) {
    static const glm::vec3 N[6] = {{0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
    for (int f = 0; f < 6; ++f) {
        const glm::vec3 n = N[f];
        glm::vec3 u = std::abs(n.y) > 0.9f ? glm::vec3(1,0,0) : glm::vec3(0,1,0);
        const glm::vec3 t = glm::normalize(glm::cross(u, n)), b = glm::cross(n, t);
        std::vector<glm::vec3> p = {
            (n - t - b) * half, (n + t - b) * half, (n + t + b) * half, (n - t + b) * half };
        previewAddMesh(g, p, {n,n,n,n}, {col,col,col,col}, {0,1,2, 0,2,3});
    }
}

void previewAddPosed(PreviewGeo& dst, const PreviewGeo& src,
                     const glm::dvec3& pos, const glm::dquat& rot) {
    const unsigned base = (unsigned)(dst.data.size() / 9);
    for (size_t i = 0; i + 8 < src.data.size(); i += 9) {
        const glm::dvec3 p = rot * glm::dvec3(src.data[i],   src.data[i+1], src.data[i+2]) + pos;
        const glm::dvec3 n = rot * glm::dvec3(src.data[i+3], src.data[i+4], src.data[i+5]);
        for (double v : { p.x, p.y, p.z, n.x, n.y, n.z }) dst.data.push_back((float)v);
        dst.data.push_back(src.data[i+6]);
        dst.data.push_back(src.data[i+7]);
        dst.data.push_back(src.data[i+8]);
    }
    for (unsigned i : src.idx) dst.idx.push_back(base + i);
}

PreviewGeo previewModelGeo(const std::string& modelPath, const glm::vec3& tint) {
    PreviewGeo g;
    if (modelPath.empty()) return g;
    Application* app = MotorInstance::getInstance().getApplication();
    if (!app) return g;
    auto* m = app->getModel(modelPath);
    if (!m) return g;
    for (const auto& mesh : m->getMeshes()) {
        std::vector<glm::vec3> p, n, c;
        p.reserve(mesh.vertex.size());
        for (const auto& v : mesh.vertex) { p.push_back(v.Position); n.push_back(v.Normal); c.push_back(tint); }
        previewAddMesh(g, p, n, c, mesh.index);
    }
    return g;
}

PreviewGeo previewPrefabGeo(const Prefab& pf, const PreviewGeoOf& geoOf) {
    PreviewGeo g;
    if (!geoOf) return g;
    std::unordered_map<std::string, PreviewGeo> mallaDe;
    for (const auto& pc : pf.pieces) {
        auto it = mallaDe.find(pc.id);
        if (it == mallaDe.end()) it = mallaDe.emplace(pc.id, geoOf(pc.id)).first;
        if (!it->second.empty()) previewAddPosed(g, it->second, pc.pos, pc.rot);
    }
    return g;
}

// ¿Sirve el preview 3D en esta máquina? Si el pipeline no se pudo crear, no tiene sentido pagar un
// render target por objeto para no ver nada y la UI pasa a un icono plano. Degradación, no arreglo.
bool previewWorks() { return !g_pipeFailed; }

uint64_t previewTexture(const std::string& key, const std::function<PreviewGeo()>& make, int px) {
    R::Device* dev = R::device();
    if (!dev || g_pipeFailed) return 0;

    // Ya generado: solo hay que traducir la textura a un id de ImGui. La traducción se rehace en cada
    // llamada A PROPÓSITO — en Vulkan el descriptor set no existe hasta que el backend de UI arranca,
    // así que cachear el id daría 0 para siempre en los items pedidos durante los primeros frames.
    if (auto it = g_cache.find(key); it != g_cache.end())
        return it->second.failed ? 0 : dev->imguiTextureId(it->second.tex);

    PreviewGeo geo = make();
    if (geo.idx.empty()) { g_cache[key] = { {}, {}, {}, {}, {}, true }; return 0; }

    R::PipelineHandle pipe = pipeline();
    if (!R::valid(pipe)) return 0;            // sin marcar como fallido el item: la culpa es del pipeline

    // El Context del frame EN CURSO: beginFrame() es idempotente (mismo command buffer), así que esto
    // graba el pase del icono dentro del frame que la UI está construyendo. Puede no haberlo todavía
    // (Vulkan sin swapchain), y entonces no se cachea nada: se reintenta en el siguiente frame.
    R::Context* ctx = dev->beginFrame();
    if (!ctx) return 0;

    // Encuadre: se centra y se escala para que quepa entero, sea cual sea su tamaño real en metros.
    glm::vec3 lo(1e9f), hi(-1e9f);
    for (size_t i = 0; i < geo.data.size(); i += 9) {
        const glm::vec3 v(geo.data[i], geo.data[i+1], geo.data[i+2]);
        lo = glm::min(lo, v); hi = glm::max(hi, v);
    }
    const glm::vec3 c = (lo + hi) * 0.5f;
    const float ext = glm::max(glm::max(hi.x-lo.x, hi.y-lo.y), hi.z-lo.z);
    const float k = ext > 1e-5f ? 1.0f / ext : 1.0f;

    Cached cc;
    R::RenderTargetDesc rtd;
    rtd.width = rtd.height = (uint32_t)px;
    rtd.colorFormats = { R::Format::RGBA8 };
    rtd.colorFilter  = R::Filter::Linear;
    rtd.hasDepth     = true;
    cc.rt = dev->createRenderTarget(rtd);
    if (!R::valid(cc.rt)) {
        SDL_Log("[Preview] no se pudo crear el render target de '%s'", key.c_str());
        g_cache[key] = { {}, {}, {}, {}, {}, true };
        return 0;
    }
    cc.tex = dev->getColorTexture(cc.rt, 0);
    cc.vb  = dev->createBuffer(R::BufferUsage::Vertex, geo.data.size() * sizeof(float), geo.data.data());
    cc.ib  = dev->createBuffer(R::BufferUsage::Index,  geo.idx.size() * sizeof(unsigned int), geo.idx.data());
    // ⚠️ UN UBO POR ITEM, no uno compartido que se reescriba entre dibujos. En Vulkan los comandos se
    // GRABAN y se ejecutan al final del frame: si varios iconos (al abrir el inventario se generan
    // muchos de golpe) compartieran el UBO, los N dibujos leerían el ÚLTIMO valor escrito y todos
    // saldrían con las matrices del último item. Es el mismo fallo que ya se midió en el bloom.
    cc.ubo = dev->createBuffer(R::BufferUsage::Uniform, sizeof(PreviewUBO), nullptr, R::BufferMemory::Dynamic);

    // Vista tres cuartos: el objeto se lee mejor de canto que de frente.
    // ⚠️ AQUÍ NO SE INVIERTE LA Y, y no es un olvido: se probó y se MIDIÓ al revés.
    //
    // El motor sí invierte la Y en la proyección para Vulkan (camera.cpp), porque la Y del clip va al
    // revés. Copiar eso aquí parece lo correcto y NO lo es: aquello corrige la imagen que se PRESENTA,
    // y esto es una textura que se MUESTREA. En una textura de color el téxel (0,0) es el de NDC y=-1
    // en GL y el de NDC y=+1 en Vulkan, así que la diferencia de convenio ya está en el propio destino
    // — invertir además la proyección la aplica DOS VECES y el icono sale del revés solo en Vulkan.
    //
    // Medido en el banco (test "icono de inventario"), misma caja alta y descentrada, 11688 px con
    // tinta en los dos backends: CON el flip la tinta caía en las filas 128..255 en GL y en las 0..127
    // en Vulkan (espejado); SIN él, los dos dan el mismo reparto. ImGui muestrea el téxel (0,0) en los
    // dos backends, así que arrays de téxeles iguales = icono igual en pantalla.
    const glm::mat4 proj = glm::ortho(-0.62f, 0.62f, -0.62f, 0.62f, -4.0f, 4.0f);

    const glm::mat4 view = glm::lookAt(glm::vec3(0.9f, 0.75f, 1.0f), glm::vec3(0.0f), glm::vec3(0,1,0));
    glm::mat4 model = glm::scale(glm::mat4(1.0f), glm::vec3(k));
    model = glm::translate(model, -c);

    PreviewUBO u{ proj * view * model, model };
    dev->updateBuffer(cc.ubo, 0, sizeof(u), &u);

    // ⚠️ Profundidad a 1, no a 0. El default del motor es reversed-Z (lejano = 0, test Greater) y el
    // icono usa una ortográfica normal con test Less: heredar el 0.0 dejaba la textura VACÍA sin dar un
    // solo error, que en pantalla es idéntico a "no hay icono". Fue el fallo mudo de la versión en GL.
    R::ClearValues cv;
    cv.clearColor = true;
    cv.color[0] = cv.color[1] = cv.color[2] = cv.color[3] = 0.0f;
    cv.clearDepth = true;
    cv.depth = 1.0f;

    // A diferencia de la versión en GL crudo, aquí NO hay que salvar ni restaurar estado: el pipeline
    // trae el suyo y el pase es explícito. Las ~40 líneas de glIsEnabled/glGet…/restaurar que hacían
    // falta para dibujar a mitad del frame de otro desaparecen con el RHI.
    ctx->beginRenderPass(cc.rt, cv);
    ctx->setViewport(0, 0, px, px);
    ctx->bindPipeline(pipe);
    ctx->bindUniformBuffer(0, cc.ubo);
    ctx->bindVertexBuffer(cc.vb);
    ctx->bindIndexBuffer(cc.ib);
    ctx->drawIndexed((uint32_t)geo.idx.size());
    ctx->endRenderPass();

    g_cache[key] = cc;
    return dev->imguiTextureId(cc.tex);
}

void clearPreviews() {
    R::Device* dev = R::device();
    if (dev) {
        for (auto& kv : g_cache) {
            Cached& x = kv.second;
            if (R::valid(x.vb))  dev->destroy(x.vb);
            if (R::valid(x.ib))  dev->destroy(x.ib);
            if (R::valid(x.ubo)) dev->destroy(x.ubo);
            // El render target es dueño de su textura de color: destruirlo la destruye (y con ella el
            // descriptor set de ImGui, que el device suelta en destroy(TextureHandle)).
            if (R::valid(x.rt))  dev->destroy(x.rt);
        }
        if (R::valid(g_pipe)) dev->destroy(g_pipe);
    }
    g_cache.clear();
    g_pipe = {};
}

} // namespace Haruka
