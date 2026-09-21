#include "renderer/post_pass.h"

#include "core/logger.h"
#include "renderer/hdr.h"
#include "renderer/shader.h"
#include "rhi/rhi_context.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_gpu_scope.h"
#include "settings/settings_manager.h"
#include "tools/profiler.h"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace Haruka::Renderer {

namespace {
// UBO del bloom (binding 2) — antes eran uniforms sueltos (glUniform1f/1i), que NO existen en
// Vulkan. Compartido por bloom_extract.frag y bloom_blur.frag (cada pase usa un campo). std140
// alinea el bloque a vec4 → padding explícito a 16 B para que C++ y GLSL coincidan.
struct BloomParams {
    float threshold;   // bright-pass: umbral de luminancia
    float horizontal;  // blur: 1 = horizontal, 0 = vertical
    float _pad0, _pad1;
};
static_assert(sizeof(BloomParams) == 16, "BloomParams std140 size mismatch");

// UBO del present/composite (binding 3) — antes uniforms sueltos (glUniform1i/2f/1f). Flags como
// FLOAT (no int/bool): el empaquetado de int/bool en std140 difiere entre GL y Vulkan. std140
// redondea el bloque a múltiplo de 16 → 5 floats ocupan 32 B, no 20: el padding es OBLIGATORIO
// o C++ y GLSL leen campos desalineados.
struct PresentParams {
    float texel[2];        // 0..8    1.0 / sceneResolution
    float bloomStrength;   // 8..12
    float fxaa;            // 12..16  >0.5 = on
    float bloom;           // 16..20  >0.5 = on
    float _pad[3];         // 20..32  (relleno std140)
};
static_assert(sizeof(PresentParams) == 32, "PresentParams std140 size mismatch");
} // namespace

void PostPass::setupQuad() {
    if (RHI::valid(m_quadBuf)) return;
    constexpr float quadVertices[] = {
        -1.0f,  1.0f,  0.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
    };
    RHI::Device* dev = RHI::device();
    m_quadBuf = dev->createBuffer(RHI::BufferUsage::Vertex, sizeof(quadVertices), quadVertices);
}

bool PostPass::begin(int windowW, int windowH, bool editorTarget) {
    const auto& gpost = Haruka::SettingsManager::get().graphics();
    // `HARUKA_RENDER_SCALE=<0.5..2>`: A/B de coste por PIXEL sin tocar los ajustes guardados. Si el
    // tiempo de GPU cae con el cuadrado de la escala, el pase es de relleno (fragmentos/overdraw);
    // si no se mueve, es de vertices o de draws.
    static const float s_envScale = [] { const char* e = std::getenv("HARUKA_RENDER_SCALE");
                                         return e ? (float)std::atof(e) : 0.0f; }();
    const float rscale = std::min(std::max(s_envScale > 0.0f ? s_envScale : gpost.renderScale, 0.5f), 2.0f);
    m_wantFXAA = (gpost.antialiasing == Haruka::Settings::AntialiasingMode::FXAA);
    // ⚠️ `HARUKA_NOBLOOM=1` apaga SOLO el bloom, no el pase: el composite es quien vuelca la escena
    // al swapchain, asi que sin el no hay nada que presentar y la medida de GPU no valdria.
    static const bool s_noBloom = [] { const char* e = std::getenv("HARUKA_NOBLOOM");
                                       return e && e[0] == '1'; }();
    m_wantBloom = gpost.bloom && !s_noBloom;
    const int renderW = std::max(1, (int)(windowW * rscale));
    const int renderH = std::max(1, (int)(windowH * rscale));
    m_active = !editorTarget && (rscale != 1.0f || m_wantFXAA || m_wantBloom);
    if (m_active && (!m_scene || m_w != renderW || m_h != renderH)) {
        m_scene = std::make_unique<HDR>((unsigned)renderW, (unsigned)renderH);
        m_w = renderW; m_h = renderH;
    }
    return m_active;
}

RHI::RenderPassHandle PostPass::scenePass() const {
    return (m_active && m_scene) ? m_scene->getPass() : RHI::RenderPassHandle{};
}

PostPass::~PostPass() { shutdown(); }

void PostPass::shutdown() {
    // Sus recursos los posee el RHI (render targets del device), no glDelete* crudos.
    if (RHI::Device* dev = RHI::device()) {
        if (RHI::valid(m_bloomExtractPSO)) dev->destroy(m_bloomExtractPSO);
        if (RHI::valid(m_bloomBlurPSO))    dev->destroy(m_bloomBlurPSO);
        for (auto& b : m_bloomUBOs) if (RHI::valid(b)) dev->destroy(b);
        for (int i = 0; i < 2; ++i) if (RHI::valid(m_bloomPass[i])) dev->destroy(m_bloomPass[i]);
        if (RHI::valid(m_presentPSO)) dev->destroy(m_presentPSO);
        if (RHI::valid(m_presentUBO)) dev->destroy(m_presentUBO);
        if (RHI::valid(m_quadBuf))    dev->destroy(m_quadBuf);
    }
    m_bloomExtractPSO = m_bloomBlurPSO = m_presentPSO = {};
    m_bloomUBOs.clear();
    m_bloomPass[0] = m_bloomPass[1] = {}; m_bloomTexH[0] = m_bloomTexH[1] = {};
    m_bloomW = m_bloomH = 0;
    m_presentUBO = {}; m_quadBuf = {};
    m_scene.reset();
    m_active = false; m_w = m_h = 0;
}

// === BLOOM: primer pase migrado a PSO/Context (ruta Vulkan) ===================================
// Antes: glUseProgram + glBindVertexArray + glUniform1f/1i + glBindTexture + glDrawArrays.
// Ahora: pipelines horneados (shader × vertex-layout × estado) + comandos por RHI::Context. Los
// uniforms SUELTOS pasaron al UBO BloomParams (binding 2) — glUniform* no existe en Vulkan.
RHI::TextureHandle PostPass::renderBloom(RHI::TextureHandle srcColorTex) {
    // Bloom runs at HALF resolution: ~4x fewer pixels through the blur ping-pong
    // for a near-identical look (bloom is low-frequency). The composite samples it
    // at full-res UV and linear-upscales.
    const int bw = std::max(1, m_w / 2), bh = std::max(1, m_h / 2);
    RHI::Device* dev = RHI::device();

    // (Re)create the two ping-pong color targets when the size changes.
    if (!RHI::valid(m_bloomPass[0]) || m_bloomW != bw || m_bloomH != bh) {
        if (RHI::valid(m_bloomPass[0])) { dev->destroy(m_bloomPass[0]); dev->destroy(m_bloomPass[1]); }
        for (int i = 0; i < 2; ++i) {
            RHI::RenderTargetDesc d;
            d.width = bw; d.height = bh; d.colorFormats = { RHI::Format::RGBA16F };
            d.colorFilter = RHI::Filter::Linear; d.hasDepth = false;
            m_bloomPass[i] = dev->createRenderTarget(d);
            m_bloomTexH[i] = dev->getColorTexture(m_bloomPass[i], 0);
        }
        m_bloomW = bw; m_bloomH = bh;
    }
    if (m_quadBuf.id == 0) setupQuad();   // crea el VBO del quad (el VAO lo aporta el PSO)

    // Pipelines horneados UNA vez: shader + vertex layout + estado de rasterizado. El VAO y el
    // glUseProgram viven DENTRO del pipeline → el llamador ya no toca GL.
    if (!RHI::valid(m_bloomExtractPSO)) {
        // OJO: createPipeline hace un ifstream CRUDO de la ruta — NO la resuelve. Hay que
        // enraizarla con el base dir de assets (lo que hace Shader por dentro). Pasar
        // "shaders/x.vert" a pelo → fichero no encontrado → shader 0 → programa sin linkar.
        const std::string vsPath = Shader::baseDir() + "shaders/screenquad.vert";
        const std::string fsExtract = Shader::baseDir() + "shaders/bloom_extract.frag";
        const std::string fsBlur    = Shader::baseDir() + "shaders/bloom_blur.frag";

        RHI::PipelineDesc pd;
        pd.vertexPath              = vsPath.c_str();
        pd.vertexLayout.strides     = { (uint32_t)(4 * sizeof(float)) };
        pd.vertexLayout.attributes = {
            { 0, 0,                 RHI::Format::RG32F },   // aPos (NDC xy)
            { 1, 2 * sizeof(float), RHI::Format::RG32F },   // aTexCoords
        };
        pd.topology     = RHI::PrimitiveTopology::TriangleStrip;
        pd.depth.test   = false;  pd.depth.write = false;   // pase fullscreen: sin depth
        pd.blend.enable = false;

        pd.fragmentPath   = fsExtract.c_str();
        m_bloomExtractPSO = dev->createPipeline(pd);
        pd.fragmentPath   = fsBlur.c_str();
        m_bloomBlurPSO    = dev->createPipeline(pd);

        m_bloomUBOs.clear();
        (void)dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(BloomParams), nullptr,
                                       RHI::BufferMemory::Dynamic);
    }
    // Si los pipelines no se pudieron crear (shader ausente/roto — createPipeline ya lo logueó),
    // degrada a SIN bloom en vez de dibujar con un programa invalido (o reintentar cada frame).
    if (!RHI::valid(m_bloomExtractPSO) || !RHI::valid(m_bloomBlurPSO)) {
        static bool s_warned = false;
        if (!s_warned) { HARUKA_LOGW("Bloom", "pipelines PSO no disponibles → bloom desactivado"); s_warned = true; }
        return {};   // handle invalido → el composite lo detecta y compone sin bloom
    }

    // GL: beginFrame() es un getter puro del Context (el swap lo sigue haciendo la app en endFrame
    // propio). Cuando el frame entero pase por el RHI, begin/endFrame subirán al bucle de render.
    RHI::Context* ctx = dev->beginFrame();
    RHI::ClearValues keep;                    // el quad cubre el target entero → no hace falta clear
    keep.clearColor = false; keep.clearDepth = false;

    // OJO (Vulkan): en GL cada draw se ejecuta al vuelo, así que reescribir el MISMO UBO entre
    // draws es correcto. En Vulkan habrá que usar offsets dinámicos o un UBO por draw (los comandos
    // se graban y se ejecutan después). Anotado para cuando entre el VKContext.
    // Un UBO por draw (ver la nota de `m_bloomUBOs` en application.h). Se crean bajo demanda y se
    // reutilizan entre frames; el tope real es 1 + 2x8 = 17 con `bloomIterations` acotado a 8.
    size_t uboSlot = 0;
    auto setParams = [&](float threshold, float horizontal) -> RHI::BufferHandle {
        if (uboSlot >= m_bloomUBOs.size())
            m_bloomUBOs.push_back(dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(BloomParams),
                                                    nullptr, RHI::BufferMemory::Dynamic));
        const RHI::BufferHandle b = m_bloomUBOs[uboSlot++];
        const BloomParams p{ threshold, horizontal, 0.0f, 0.0f };
        dev->updateBuffer(b, 0, sizeof(p), &p);
        return b;
    };

    // 1. Bright-pass: scene color -> tex[0].
    const RHI::BufferHandle uboExtract =
        setParams(Haruka::SettingsManager::get().graphics().bloomThreshold, 0.0f);
    ctx->beginRenderPass(m_bloomPass[0], keep);   // bindea FBO + viewport(bw,bh)
    ctx->bindPipeline(m_bloomExtractPSO);
    ctx->bindVertexBuffer(m_quadBuf);
    ctx->bindUniformBuffer(2, uboExtract);
    ctx->bindTexture(0, srcColorTex);
    ctx->draw(4);

    // 2. Separable Gaussian: N iterations of horizontal+vertical, ping-ponging
    //    tex[1] <-> tex[0]. Result ends up in tex[0].
    RHI::TextureHandle src = m_bloomTexH[0];
    // El radio del halo sale de aquí (σ ∝ √N). Ya no es un literal: es ajuste, junto al umbral y la
    // fuerza. Acotado 1..8 porque por debajo de 1 no hay blur y por encima de 8 el halo se come la
    // pantalla sin aportar nada — y ambos extremos son fáciles de escribir por error en el .json.
    const int iterations = std::clamp(Haruka::SettingsManager::get().graphics().bloomIterations, 1, 8);
    for (int i = 0; i < iterations; ++i) {
        const RHI::BufferHandle uboH = setParams(0.0f, 1.0f);   // horizontal -> tex[1]
        ctx->beginRenderPass(m_bloomPass[1], keep);
        ctx->bindPipeline(m_bloomBlurPSO);
        ctx->bindVertexBuffer(m_quadBuf);
        ctx->bindUniformBuffer(2, uboH);
        ctx->bindTexture(0, src);
        ctx->draw(4);

        const RHI::BufferHandle uboV = setParams(0.0f, 0.0f);   // vertical -> tex[0]
        ctx->beginRenderPass(m_bloomPass[0], keep);
        ctx->bindPipeline(m_bloomBlurPSO);
        ctx->bindVertexBuffer(m_quadBuf);
        ctx->bindUniformBuffer(2, uboV);
        ctx->bindTexture(0, m_bloomTexH[1]);
        ctx->draw(4);
        src = m_bloomTexH[0];
    }
    ctx->endRenderPass();

    return m_bloomTexH[0];   // handle: el composite (también PSO) lo bindea por el Context
}

void PostPass::composite(int width, int height) {
if (m_active && m_scene) {
    HARUKA_PROFILE("post.composite(FXAA+bloom+upscale)"); HARUKA_GPU_SCOPE("post.composite(FXAA+bloom+upscale)");
    RHI::Device* dev = RHI::device();
    if (m_quadBuf.id == 0) setupQuad();   // VBO del quad (el VAO lo aporta el PSO)

    // Pipeline horneado 1 vez. Dibuja a PANTALLA → sin depth ni blend.
    if (!RHI::valid(m_presentPSO)) {
        const std::string vsPath = Shader::baseDir() + "shaders/screenquad.vert";
        const std::string fsPath = Shader::baseDir() + "shaders/post_present.frag";
        RHI::PipelineDesc pd;
        pd.vertexPath              = vsPath.c_str();
        pd.fragmentPath            = fsPath.c_str();
        pd.vertexLayout.strides     = { (uint32_t)(4 * sizeof(float)) };
        pd.vertexLayout.attributes = {
            { 0, 0,                 RHI::Format::RG32F },   // aPos
            { 1, 2 * sizeof(float), RHI::Format::RG32F },   // aTexCoords
        };
        pd.topology     = RHI::PrimitiveTopology::TriangleStrip;
        pd.depth.test   = false;  pd.depth.write = false;
        pd.blend.enable = false;
        m_presentPSO = dev->createPipeline(pd);
        m_presentUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PresentParams), nullptr,
                                         RHI::BufferMemory::Dynamic);
    }

    if (RHI::valid(m_presentPSO)) {
        // El bloom se construye ANTES (rebindea FBOs/viewport) y devuelve su handle.
        RHI::TextureHandle bloomTex;
        if (m_wantBloom) bloomTex = renderBloom(m_scene->getColorTextureHandle());
        const bool haveBloom = m_wantBloom && RHI::valid(bloomTex);

        PresentParams p{};
        p.texel[0]      = 1.0f / (float)m_w;
        p.texel[1]      = 1.0f / (float)m_h;
        p.bloomStrength = Haruka::SettingsManager::get().graphics().bloomStrength;
        p.fxaa          = m_wantFXAA  ? 1.0f : 0.0f;
        p.bloom         = haveBloom ? 1.0f : 0.0f;
        dev->updateBuffer(m_presentUBO, 0, sizeof(p), &p);

        RHI::Context* ctx = dev->beginFrame();
        // ⚠️ SE LIMPIA, aunque el quad cubra la pantalla entera. Antes ponía `clearColor=false`
        // razonando justo eso, y en OpenGL es correcto. En Vulkan NO: desde que las render
        // passes tienen variante `loadOp LOAD` (necesaria para que el motor pueda reabrir un
        // pase sin borrar lo dibujado), no limpiar significa CONSERVAR lo que había en ESA
        // imagen del swapchain — que con 3 imágenes rotando es el frame de hace 2 o 3 turnos.
        // Cualquier píxel que el quad no cubra exactamente enseña ese frame viejo: el "frame
        // fantasma". Limpiar aquí no cuesta nada (el quad lo sobreescribe igual) y elimina la
        // posibilidad por construcción.
        RHI::ClearValues clr;
        clr.clearColor = true;
        clr.color[0] = 0.0f; clr.color[1] = 0.0f; clr.color[2] = 0.0f; clr.color[3] = 1.0f;
        clr.clearDepth = true; clr.depth = 0.0f;      // reversed-Z: 0 = lejano

        ctx->beginRenderPass(RHI::RenderPassHandle{}, clr);  // id 0 = backbuffer (pantalla)
        ctx->setViewport(0, 0, (int)width, (int)height);      // el pass a pantalla NO fija viewport
        ctx->bindPipeline(m_presentPSO);
        ctx->bindVertexBuffer(m_quadBuf);
        ctx->bindUniformBuffer(3, m_presentUBO);
        ctx->bindTexture(0, m_scene->getColorTextureHandle());
        // El sampler u_bloomTex se lee SIEMPRE en el shader aunque u_bloom sea 0; bindea algo
        // válido para no dejar la unidad 1 con basura de un pase anterior.
        ctx->bindTexture(1, haveBloom ? bloomTex : m_scene->getColorTextureHandle());
        ctx->draw(4);
        ctx->endRenderPass();
    }

    // RHI migration: GL state transitions removed
}
}

} // namespace Haruka::Renderer
