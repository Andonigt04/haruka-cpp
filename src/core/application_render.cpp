// Application — render pipeline.
// The per-frame work: build the render queue, the deferred/forward object pass,
// terrain/islands/water passes, the standalone post-processing composite and the
// GPU timer. Lifecycle/orchestration lives in application.cpp; the GL asset
// caches in application_assets.cpp.

#include "core/terrain/gpu_heightfield.h"   // planetFieldsSSBO (F3): el agua consulta el campo
#include "application.h"
#include "application_internal.h"
#include "rhi/rhi_context.h"   // ruta PSO: comandos de dibujo del frame (bloom migrado)

#include <algorithm>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>

#include "game/planetary_system.h"
#include "core/components/mesh_renderer_component.h"
#include "renderer/model.h"
#include "renderer/simple_mesh.h"
#include "tools/object_types.h"
#include "tools/profiler.h"
#include "settings/settings_manager.h"
#include "io/image_writer.h"
#include "core/asset_paths.h"

#include <vector>
#include <string>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <iostream>

namespace Haruka { namespace Core {

using AppInternal::g_sceneRenderQueue;

// std140-compatible structs mirroring the UBO declarations in the shaders.
// Any change to the GLSL UBO layout must be reflected here.
namespace {
struct alignas(16) PerFrameUBOData {
    glm::mat4 view;
    glm::mat4 projection;
    glm::vec3 cameraPos;      float _pad0;
    glm::vec3 sunDirection;   float _pad1;
    glm::vec3 sunLightColor;  float ambientStrength;
    int enableHDR;
    int enableBloom;
    int enableSSAO;
    int enableIBL;
    int enableShadows;
    int _pad3[3];
    // Luz de luna (2ª luz, añadida al final → no mueve offsets previos). Los shaders
    // que no la usan pueden omitir estos campos de su bloque UBO.
    glm::vec3 moonDirection;  float moonIntensity;  // dir hacia la Luna + brillo (fase)
    glm::vec3 moonLightColor; float _pad4;          // color azulado tenue
};
static_assert(sizeof(PerFrameUBOData) == 240, "PerFrameUBOData std140 size mismatch");

struct alignas(16) PerObjectUBOData {
    glm::mat4 model;
    glm::vec4 baseColorAndPlanetRadius; // rgb=color, a=planetRadius
    glm::vec4 planetCenterAndFlag;      // xyz=planetCenter, w=useProceduralTerrain
};
static_assert(sizeof(PerObjectUBOData) == 96, "PerObjectUBOData std140 size mismatch");

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

// UBO del cielo (binding 5) — compartido por sky.vert (la mat4) y sky.frag (el resto). Antes eran
// uniforms sueltos (glUniformMatrix4fv/3fv/1f). Truco std140: un vec3 tiene alineación 16 pero
// tamaño 12 → el float que le SIGUE cabe en el mismo slot de 16 B. Por eso los pares vec3+float.
struct SkyParams {
    glm::mat4 invViewProjRot; //   0..64
    glm::vec3 sunDir;         //  64..76
    float     sunElev;        //  76..80  (relleno del vec3 anterior)
    glm::vec3 up;             //  80..92
    float     atmo;           //  92..96
    glm::vec3 sunColor;       //  96..108
    float     _pad;           // 108..112
};
static_assert(sizeof(SkyParams) == 112, "SkyParams std140 size mismatch");
} // namespace

void Application::setupQuad() {
    if (quadVAO != 0 || quadVBO != 0) return;

    constexpr float quadVertices[] = {
        -1.0f,  1.0f,  0.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
    };

    glGenVertexArrays(1, &quadVAO);
    glBindVertexArray(quadVAO);
    {
        RHI::Device* dev = RHI::device();
        m_quadBuf = dev->createBuffer(RHI::BufferUsage::Vertex, sizeof(quadVertices), quadVertices);
        quadVBO = dev->nativeBuffer(m_quadBuf);
        glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    }
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray(0);
}

void Application::buildRenderQueue() {
    HARUKA_PROFILE("buildRenderQueue");

    if (!_currentScene) { g_sceneRenderQueue.clear(); return; }

    // Cache the STATIC scene classification (kind/lod per object), which only
    // changes when the object set changes — not every frame. The expensive part
    // is the per-object string lowercasing + substring matching; transforms are
    // read live from the shared_ptr at draw time, so movement still updates.
    const size_t objCount = _currentScene->getAllObjects().size();
    if (m_renderQueueDirty || objCount != m_renderQueueObjCount) {
        m_staticRenderQueue = buildSceneRenderQueue(*_currentScene);
        m_renderQueueObjCount = objCount;
        m_renderQueueDirty = false;
    }
    // Copy the cached static prefix (reuses g_sceneRenderQueue's buffer after the
    // first frame — no realloc), then append per-frame network ghosts below.
    g_sceneRenderQueue = m_staticRenderQueue;

#ifdef HARUKA_NETWORK
    m_ghostObjects.clear();
    for (const auto& ghost : m_dgs.pollGhosts()) {
        Haruka::SceneObject obj;
        obj.name = "ghost_" + std::to_string(ghost.uuid);
        obj.type = "Character";
        obj.position = Haruka::WorldPos(
            ghost.chunkX * Haruka::Units::KM + ghost.pos[0],
            ghost.chunkY * Haruka::Units::KM + ghost.pos[1],
            ghost.chunkZ * Haruka::Units::KM + ghost.pos[2]
        );
        m_ghostObjects.push_back(std::move(obj));
    }
    for (const auto& obj : m_ghostObjects) {
        g_sceneRenderQueue.push_back({ &obj, Haruka::RenderKind::Primitive, Haruka::PrimitiveType::CAPSULE });
    }
#endif

    _iTotalDrawCalls = static_cast<int>(g_sceneRenderQueue.size());
    _iRenderedDrawCalls = _iTotalDrawCalls;
}

// === BLOOM: primer pase migrado a PSO/Context (ruta Vulkan) ===================================
// Antes: glUseProgram + glBindVertexArray + glUniform1f/1i + glBindTexture + glDrawArrays.
// Ahora: pipelines horneados (shader × vertex-layout × estado) + comandos por RHI::Context. Los
// uniforms SUELTOS pasaron al UBO BloomParams (binding 2) — glUniform* no existe en Vulkan.
RHI::TextureHandle Application::renderBloom(RHI::TextureHandle srcColorTex) {
    // Bloom runs at HALF resolution: ~4x fewer pixels through the blur ping-pong
    // for a near-identical look (bloom is low-frequency). The composite samples it
    // at full-res UV and linear-upscales.
    const int bw = std::max(1, m_postW / 2), bh = std::max(1, m_postH / 2);
    RHI::Device* dev = RHI::device();

    // (Re)create the two ping-pong color targets when the size changes.
    if (m_bloomFBO[0] == 0 || m_bloomW != bw || m_bloomH != bh) {
        if (RHI::valid(m_bloomPass[0])) { dev->destroy(m_bloomPass[0]); dev->destroy(m_bloomPass[1]); }
        for (int i = 0; i < 2; ++i) {
            RHI::RenderTargetDesc d;
            d.width = bw; d.height = bh; d.colorFormats = { RHI::Format::RGBA16F };
            d.colorFilter = RHI::Filter::Linear; d.hasDepth = false;
            m_bloomPass[i] = dev->createRenderTarget(d);
            m_bloomFBO[i]  = dev->nativeFramebuffer(m_bloomPass[i]);
            m_bloomTexH[i] = dev->getColorTexture(m_bloomPass[i], 0);   // handle (Context) — sin id GL
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

        m_bloomUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(BloomParams), nullptr,
                                       RHI::BufferMemory::Dynamic);
    }
    // Si los pipelines no se pudieron crear (shader ausente/roto — createPipeline ya lo logueó),
    // degrada a SIN bloom en vez de dibujar con un programa invalido (o reintentar cada frame).
    if (!RHI::valid(m_bloomExtractPSO) || !RHI::valid(m_bloomBlurPSO)) {
        static bool s_warned = false;
        if (!s_warned) { std::fprintf(stderr, "[bloom] pipelines PSO no disponibles → bloom desactivado\n"); s_warned = true; }
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
    auto setParams = [&](float threshold, float horizontal) {
        const BloomParams p{ threshold, horizontal, 0.0f, 0.0f };
        dev->updateBuffer(m_bloomUBO, 0, sizeof(p), &p);
    };

    // 1. Bright-pass: scene color -> tex[0].
    setParams(Haruka::SettingsManager::get().graphics().bloomThreshold, 0.0f);
    ctx->beginRenderPass(m_bloomPass[0], keep);   // bindea FBO + viewport(bw,bh)
    ctx->bindPipeline(m_bloomExtractPSO);
    ctx->bindVertexBuffer(m_quadBuf);
    ctx->bindUniformBuffer(2, m_bloomUBO);
    ctx->bindTexture(0, srcColorTex);
    ctx->draw(4);

    // 2. Separable Gaussian: N iterations of horizontal+vertical, ping-ponging
    //    tex[1] <-> tex[0]. Result ends up in tex[0].
    RHI::TextureHandle src = m_bloomTexH[0];
    const int iterations = 5;
    for (int i = 0; i < iterations; ++i) {
        setParams(0.0f, 1.0f);                          // horizontal -> tex[1]
        ctx->beginRenderPass(m_bloomPass[1], keep);
        ctx->bindPipeline(m_bloomBlurPSO);
        ctx->bindVertexBuffer(m_quadBuf);
        ctx->bindUniformBuffer(2, m_bloomUBO);
        ctx->bindTexture(0, src);
        ctx->draw(4);

        setParams(0.0f, 0.0f);                          // vertical -> tex[0]
        ctx->beginRenderPass(m_bloomPass[0], keep);
        ctx->bindPipeline(m_bloomBlurPSO);
        ctx->bindVertexBuffer(m_quadBuf);
        ctx->bindUniformBuffer(2, m_bloomUBO);
        ctx->bindTexture(0, m_bloomTexH[1]);
        ctx->draw(4);
        src = m_bloomTexH[0];
    }
    ctx->endRenderPass();

    // Transición: partes del frame que aún dibujan en GL directo dan por hecho VAO 0 y depth ON.
    // Desaparece cuando TODOS los pases usen el PSO.
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
    return m_bloomTexH[0];   // handle: el composite (también PSO) lo bindea por el Context
}

void Application::renderFrameContent() {
    // Editor path calls this externally (no standalone loop) → reset here.
    // Standalone loop resets at its own frame boundary so game.onUpdate is measured.
    if (_editorTarget) Haruka::Profiler::get().newFrame();
    HARUKA_PROFILE("renderFrameContent");
    const uint32_t width  = _window ? _window->getWidth()  : static_cast<uint32_t>(m_editorViewportW);
    const uint32_t height = _window ? _window->getHeight() : static_cast<uint32_t>(m_editorViewportH);

    // --- GPU timer (double-buffered TIME_ELAPSED query) ---
    // Measures real GPU work per frame — the metric the CPU profiler can't see.
    // We read the PREVIOUS frame's result (already finished) to avoid a stall.
    if (m_gpuTimerQuery[0] == 0) glGenQueries(2, m_gpuTimerQuery);
    int prev = m_gpuTimerFrame ^ 1;
    // Only query a buffer that has actually been closed with glEndQuery before;
    // otherwise the id is "invalid or active" (GL_INVALID_OPERATION) on early frames.
    if (m_gpuTimerIssued[prev]) {
        GLint avail = 0;
        glGetQueryObjectiv(m_gpuTimerQuery[prev], GL_QUERY_RESULT_AVAILABLE, &avail);
        if (avail) {
            GLuint64 ns = 0;
            glGetQueryObjectui64v(m_gpuTimerQuery[prev], GL_QUERY_RESULT, &ns);
            m_lastGpuMs = (float)(ns / 1.0e6);
            Haruka::Profiler::get().add("GPU (frame)", m_lastGpuMs);
        }
    }
    glBeginQuery(GL_TIME_ELAPSED, m_gpuTimerQuery[m_gpuTimerFrame]);

    if (_editorTarget) {
        _editorTarget->bindForWriting();
    }

    // --- Standalone post-processing target ---------------------------------
    // Route the 3D scene through an offscreen HDR target when any screen-space
    // effect or non-1.0 render scale is active. With everything off this stays
    // false → the scene renders straight to the screen exactly as before. The
    // editor path (its own _editorTarget) is never rerouted.
    const auto& gpost  = Haruka::SettingsManager::get().graphics();
    const float rscale = std::min(std::max(gpost.renderScale, 0.5f), 2.0f);
    const bool  wantFXAA = (gpost.antialiasing == Haruka::Settings::AntialiasingMode::FXAA);
    const bool  wantBloom = gpost.bloom;
    const int   renderW  = std::max(1, (int)(width  * rscale));
    const int   renderH  = std::max(1, (int)(height * rscale));
    m_postActive = !_editorTarget && (rscale != 1.0f || wantFXAA || wantBloom);
    m_sceneTargetFBO = 0;
    if (m_postActive) {
        if (!_postScene || m_postW != renderW || m_postH != renderH) {
            _postScene = std::make_unique<HDR>((unsigned)renderW, (unsigned)renderH);
            m_postW = renderW; m_postH = renderH;
        }
        _postScene->bindForWriting();
        m_sceneTargetFBO = _postScene->getFBO();
    }

    glViewport(0, 0, m_postActive ? (uint32_t)renderW : width,
                     m_postActive ? (uint32_t)renderH : height);
    // Mecánica celeste: el WorldSystem usa el planeta REAL (su PlanetarySystem propio
    // está vacío). Le pasamos centro/radio del planeta activo y avanzamos día/noche +
    // luna + marea + viento cada frame (WorldSystem::update no se llama).
    if (_worldSystem && _planetarySystem) {
        glm::dvec3 pc; double pr; uint32_t psd; float prl;
        if (_planetarySystem->getActivePlanet(pc, pr, psd, prl))
            _worldSystem->setActivePlanet(pc, pr);
        _worldSystem->advanceCelestial(deltaTime > 0.0f ? (double)deltaTime : 0.016);
    }

    // NEAR PLANE DINÁMICO: pegado a cualquier superficie (alt ≤ 2 km) near=0.1 (precisión cercana
    // para terreno/objetos/ítem en mano). En órbita el near sube con la altitud → la precisión del
    // depth lejano mejora ~millones× → se acaba el z-fight agua↔lecho desde el espacio. Usa el
    // planeta MÁS CERCANO (no el home) para no recortar superficies cercanas en un sobrevuelo.
    if (_camera && _planetarySystem) {
        const glm::dvec3 camD = glm::dvec3(_camera->position);
        double nearestAlt = 1e30;
        for (const auto& pl : _planetarySystem->getPlanets()) {
            double a = glm::length(camD - pl.position) - pl.radius;
            if (a < nearestAlt) nearestAlt = a;
        }
        float dynNear = 0.1f;
        if (nearestAlt > 2000.0)
            dynNear = glm::clamp((float)((nearestAlt - 2000.0) * 0.05), 0.1f, (float)(nearestAlt * 0.5));
        _camera->setNearPlane(dynNear);
    }

    // Cielo atmosférico: color por elevación solar + altitud (azul de día → cálido al
    // amanecer/atardecer → oscuro de noche → negro en el espacio). Fallback oscuro.
    glm::vec3 sky(0.01f);
    if (_worldSystem && _camera) sky = _worldSystem->getSkyColor(glm::dvec3(_camera->position));
    glClearColor(sky.r, sky.g, sky.b, 1.0f);
    glClearDepth(0.0);   // REVERSED-Z: el "infinito" es 0, no 1
    glDepthMask(GL_TRUE);  // el clear de depth exige write habilitado
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Pase de cielo procedural (gradiente + sol + estrellas) como FONDO: triángulo
    // fullscreen SIN escribir profundidad → el terreno/objetos se pintan encima.
    if (_worldSystem && _camera && _planetarySystem) {
        glm::dvec3 pc; double pr; uint32_t psd; float prl;
        if (_planetarySystem->getActivePlanet(pc, pr, psd, prl)) {
            // === PASE 3 MIGRADO A PSO/Context ===
            // Sin VBO: el triángulo fullscreen sale de gl_VertexID → pipeline SIN vertex layout
            // (createPipeline crea igualmente el VAO vacío que bindPipeline ata) y draw(3).
            RHI::Device* skyDev = RHI::device();
            if (!RHI::valid(m_skyPSO)) {
                const std::string vsPath = Shader::baseDir() + "shaders/sky.vert";
                const std::string fsPath = Shader::baseDir() + "shaders/sky.frag";
                RHI::PipelineDesc pd;
                pd.vertexPath   = vsPath.c_str();
                pd.fragmentPath = fsPath.c_str();
                pd.topology     = RHI::PrimitiveTopology::Triangles;  // vertexLayout vacío a propósito
                // Fondo: sin depth test → GL tampoco ESCRIBE depth (con el test off no se actualiza
                // el z-buffer), así que el terreno/objetos se pintan encima sin más.
                pd.depth.test   = false;  pd.depth.write = false;
                pd.blend.enable = false;
                m_skyPSO = skyDev->createPipeline(pd);
                m_skyUBO = skyDev->createBuffer(RHI::BufferUsage::Uniform, sizeof(SkyParams), nullptr,
                                                RHI::BufferMemory::Dynamic);
            }
            // Si el pipeline no se creó (shader roto), saltar el cielo: el clearColor deja un fondo
            // de respaldo y el resto de la escena renderiza normal.
            if (RHI::valid(m_skyPSO)) {
            const glm::dvec3 camD = glm::dvec3(_camera->position);
            glm::dvec3 up = camD - pc; double ul = glm::length(up);
            up = (ul > 1e-9) ? up / ul : glm::dvec3(0, 1, 0);
            glm::vec3 sunDir  = _worldSystem->getDominantLightDirection(camD);
            glm::vec3 sunCol  = _worldSystem->getDominantLightColor(camD);
            float     sunElev = (float)glm::dot(glm::dvec3(sunDir), up);
            float     alt     = (float)(ul - pr);
            float     atmo    = 1.0f - glm::smoothstep(0.0f, (float)(pr * 0.02), alt);

            float aspectS = (float)(m_postActive ? renderW : width)
                          / (float)(m_postActive ? renderH : height);
            glm::mat4 viewRot  = glm::mat4(glm::mat3(_camera->getViewMatrix()));
            glm::mat4 invVPRot = glm::inverse(_camera->getProjectionMatrix(aspectS) * viewRot);

            SkyParams sp{};
            sp.invViewProjRot = invVPRot;
            sp.sunDir         = sunDir;
            sp.sunElev        = sunElev;
            sp.up             = glm::vec3(up);
            sp.atmo           = atmo;
            sp.sunColor       = sunCol;
            skyDev->updateBuffer(m_skyUBO, 0, sizeof(sp), &sp);

            const GLboolean depthWas = glIsEnabled(GL_DEPTH_TEST);
            RHI::Context* ctx = skyDev->beginFrame();
            // El target de escena (backbuffer o _postScene) ya está bindeado y limpiado por el
            // código de arriba → reabrimos el MISMO pass sin clear (borraría la escena).
            RHI::ClearValues keep; keep.clearColor = false; keep.clearDepth = false;
            ctx->beginRenderPass(m_postActive && _postScene ? _postScene->getPass()
                                                            : RHI::RenderPassHandle{}, keep);
            if (!(m_postActive && _postScene))                       // el pass a pantalla NO fija viewport
                ctx->setViewport(0, 0, (int)width, (int)height);
            ctx->bindPipeline(m_skyPSO);                             // programa + depth off (no escribe z)
            ctx->bindUniformBuffer(5, m_skyUBO);
            ctx->draw(3);                                            // sin vertex buffer: gl_VertexID
            ctx->endRenderPass();

            // Transición: los pases de escena siguientes van en GL directo y esperan depth ON.
            glBindVertexArray(0);
            glDepthMask(GL_TRUE);
            if (depthWas) glEnable(GL_DEPTH_TEST);
            } // if (RHI::valid(m_skyPSO))
        }
    }

    _iTotalDrawCalls    = static_cast<int>(g_sceneRenderQueue.size());
    _iRenderedDrawCalls = 0;
    _iTotalVertices     = 0;
    _iTotalTriangles    = 0;
    _iRenderedVertices  = 0;
    _iRenderedTriangles = 0;

    if (_currentScene && _camera) {
        // Lazy-create UBOs (dinámicos; el update por frame sigue con glBufferSubData/glBindBufferBase).
        RHI::Device* uboDev = RHI::device();
        if (m_uboPerFrame == 0) {
            m_uboPerFrameH = uboDev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PerFrameUBOData), nullptr, RHI::BufferMemory::Dynamic);
            m_uboPerFrame = uboDev->nativeBuffer(m_uboPerFrameH);
        }
        if (m_uboPerObject == 0) {
            m_uboPerObjectH = uboDev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PerObjectUBOData), nullptr, RHI::BufferMemory::Dynamic);
            m_uboPerObject = uboDev->nativeBuffer(m_uboPerObjectH);
        }

        const bool useFinalLook = getRenderFeatureHDR() || getRenderFeatureBloom()
                                || getRenderFeatureSSAO() || getRenderFeatureIBL()
                                || getRenderFeatureShadows();
        if (!_mainShader || _mainShaderUsesFinalLook != useFinalLook) {
            _mainShader = std::make_unique<Shader>(
                "shaders/simple.vert",
                useFinalLook ? "shaders/final.frag" : "shaders/preview.frag"
            );
            _mainShaderUsesFinalLook = useFinalLook;
        }
        // (Los programas de planet.* y water.* los poseen ahora los PSO del TerrainRenderer y del
        //  WaterRenderer — cada pase ata su propio pipeline; ya no hay Shader suelto que bindear.)

        // === PASE 4 MIGRADO A PSO/Context: los OBJETOS de la escena. ===
        // El pipeline hornea shader + layout `Vertex` + estado (depth on). Se recrea solo si
        // cambia la variante de fragment (final.frag <-> preview.frag).
        if (!RHI::valid(m_scenePSO) || m_scenePSOFinalLook != useFinalLook) {
            if (RHI::valid(m_scenePSO)) uboDev->destroy(m_scenePSO);
            const std::string vsPath = Shader::baseDir() + "shaders/simple.vert";
            const std::string fsPath = Shader::baseDir() +
                (useFinalLook ? "shaders/final.frag" : "shaders/preview.frag");
            using V = Haruka::Renderer::Vertex;
            RHI::PipelineDesc pd;
            pd.vertexPath              = vsPath.c_str();
            pd.fragmentPath            = fsPath.c_str();
            pd.vertexLayout.strides     = { (uint32_t)(sizeof(V)) };
            pd.vertexLayout.attributes = {
                { 0, (uint32_t)offsetof(V, Position),  RHI::Format::RGB32F },
                { 1, (uint32_t)offsetof(V, Normal),    RHI::Format::RGB32F },
                { 2, (uint32_t)offsetof(V, TexCoords), RHI::Format::RG32F  },
                { 3, (uint32_t)offsetof(V, Tangent),   RHI::Format::RGB32F },
                { 4, (uint32_t)offsetof(V, Bitangent), RHI::Format::RGB32F },
            };
            pd.topology     = RHI::PrimitiveTopology::Triangles;
            pd.depth.test   = true;  pd.depth.write = true;   // escena sólida
            pd.blend.enable = false;
            pd.cull         = RHI::CullMode::Back;   // lo hacía el glEnable(GL_CULL_FACE) global
            m_scenePSO = uboDev->createPipeline(pd);
            m_scenePSOFinalLook = useFinalLook;
        }

        RHI::Context* sceneCtx = uboDev->beginFrame();
        if (RHI::valid(m_scenePSO)) {
            // El target de escena ya está bindeado y limpiado arriba → reabrir el MISMO pass SIN
            // clear (borraría el cielo ya pintado).
            RHI::ClearValues keep; keep.clearColor = false; keep.clearDepth = false;
            sceneCtx->beginRenderPass(m_postActive && _postScene ? _postScene->getPass()
                                                                 : RHI::RenderPassHandle{}, keep);
            if (!(m_postActive && _postScene))          // el pass a pantalla NO fija viewport
                sceneCtx->setViewport(0, 0, (int)width, (int)height);
            sceneCtx->bindPipeline(m_scenePSO);         // programa + depth on (absorbe glUseProgram)
            sceneCtx->bindUniformBuffer(0, m_uboPerFrameH);
            sceneCtx->bindUniformBuffer(1, m_uboPerObjectH);
        }

        // Upload per-frame UBO
        const float      aspect       = (height > 0u) ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
        const glm::vec3  cameraOrigin = glm::vec3(_camera->position);

        PerFrameUBOData frameData{};
        frameData.view            = glm::mat4(glm::mat3(_camera->getViewMatrix()));
        frameData.projection      = _camera->getProjectionMatrix(aspect);
        frameData.cameraPos       = cameraOrigin;
        // Sun direction and color come from the WorldSystem (supports multiple stars).
        // Lazy sync in case the scene was loaded before the WorldSystem was ready.
        if (_worldSystem && _currentScene && _worldSystem->getBodies().empty())
            _worldSystem->syncFromScene(*_currentScene);

        if (_worldSystem && !_worldSystem->getBodies().empty())
            frameData.sunDirection  = _worldSystem->getDominantLightDirection(glm::dvec3(cameraOrigin));
        else
            frameData.sunDirection  = glm::vec3(-0.55f, 0.69f, -0.41f); // reasonable fallback
        frameData.sunLightColor = (_worldSystem && !_worldSystem->getBodies().empty())
            ? _worldSystem->getDominantLightColor(glm::dvec3(cameraOrigin))
            : glm::vec3(1.0f, 0.98f, 0.95f);
        // Ambiente por ATMÓSFERA: de día luz ambiental clara, de noche casi nada (la
        // luna aporta el relleno). Suaviza la transición con la elevación solar.
        {
            float el  = _worldSystem ? _worldSystem->getSunElevation(glm::dvec3(cameraOrigin)) : 1.0f;
            float day = glm::smoothstep(-0.10f, 0.25f, el);
            // Suelo nocturno subido (0.04→0.11): el lado noche era casi negro → parecía "media
            // planeta sin dibujar". Ahora se ve TENUE (luz de estrellas/cielo), sigue siendo noche.
            frameData.ambientStrength = glm::mix(0.11f, 0.28f, day);
        }
        // Viento atmosférico → arrastre aerodinámico de la física (por cuerpo, barato).
        if (_physicsEngine && _worldSystem)
            _physicsEngine->setWind(_worldSystem->getWind(glm::dvec3(cameraOrigin)));
        // Avanza el motor de física con TIMESTEP FIJO (determinista). Hoy solo procesa cuerpos
        // DINÁMICOS (el jugador aún es kinemático → no afecta); lo activa de verdad la Fase 2.
        if (_physicsEngine)
            _physicsEngine->advance(deltaTime > 0.0f ? (double)deltaTime : 0.016);
        // Luz de luna (2ª luz): dirección + brillo por fase (WorldSystem), color azulado.
        {
            glm::vec3 moonDir(0.0f, 1.0f, 0.0f); float moonI = 0.0f;
            if (_worldSystem) _worldSystem->getMoonLight(glm::dvec3(cameraOrigin), moonDir, moonI);
            frameData.moonDirection  = moonDir;
            frameData.moonIntensity  = moonI;
            frameData.moonLightColor = glm::vec3(0.55f, 0.62f, 0.85f); // azul plateado
        }
        // Only advertise a feature if its GPU resources are actually allocated.
        // Enabling a flag without the corresponding FBO/texture bound causes
        // undefined behaviour in final.frag (samples from empty texture units).
        frameData.enableHDR       = (int)(getRenderFeatureHDR()     && _hdr    != nullptr);
        frameData.enableBloom     = (int)(getRenderFeatureBloom()   && _bloom  != nullptr);
        frameData.enableSSAO      = (int)(getRenderFeatureSSAO()    && _ssao   != nullptr);
        frameData.enableIBL       = (int)(getRenderFeatureIBL()     && _ibl    != nullptr);
        frameData.enableShadows   = (int)(getRenderFeatureShadows() && _shadow != nullptr);

        uboDev->updateBuffer(m_uboPerFrameH, 0, sizeof(PerFrameUBOData), &frameData);

        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);

        int renderedDrawCalls = 0;
        int renderedVertices  = 0;
        int renderedTriangles = 0;

        { HARUKA_PROFILE("scene.objects.draw");
        for (const auto& command : g_sceneRenderQueue) {
            const auto* obj = command.object;
            if (!obj) continue;

            // "Emisivo" (estrella, sin sombreado) = el cuerpo EMITE luz. Así un
            // CelestialBody que NO emite (la Luna) queda SOMBREADO por el Sol → se
            // ven sus cráteres y fases, en vez de salir a pleno brillo.
            const bool isStar = obj->flags.castLight;

            // Distance LOD/cull for scene objects (they have no chunk LOD like terrain):
            // skip far props/models so they don't cost draw calls at any range. Scale-aware
            // (bigger objects stay visible farther). Stars/celestial bodies are never culled
            // (they must show from orbit/space).
            if (!isStar) {
                double objDist  = glm::length(obj->position - _camera->position);
                double maxScale = std::max({obj->scale.x, obj->scale.y, obj->scale.z});
                double maxDist  = 500.0 + maxScale * 300.0;   // 1 m → ~800 m, 50 m → ~15 km
                if (objDist > maxDist) continue;
            }

            glm::vec3 baseColor = glm::vec3(obj->color);
            if (glm::length(baseColor) < 0.001f) baseColor = glm::vec3(0.75f, 0.76f, 0.80f);

            // Stars emit from their light component color, not the default object color.
            if (isStar && obj->components.contains("light")) {
                const auto& lc = obj->components["light"];
                if (lc.contains("color") && lc["color"].is_array() && lc["color"].size() >= 3)
                    baseColor = glm::vec3(lc["color"][0].get<float>(),
                                         lc["color"][1].get<float>(),
                                         lc["color"][2].get<float>());
            }

            PerObjectUBOData objData{};
            objData.model                    = AppInternal::getTransformMatrix(*obj, _camera->position);
            objData.baseColorAndPlanetRadius = glm::vec4(baseColor, 1.0f);
            // w = 0: normal, 1: procedural terrain, 2: emissive star (no diffuse shading)
            objData.planetCenterAndFlag      = glm::vec4(0.0f, 0.0f, 0.0f, isStar ? 2.0f : 0.0f);

            uboDev->updateBuffer(m_uboPerObjectH, 0, sizeof(PerObjectUBOData), &objData);

            switch (command.kind) {
                case Haruka::RenderKind::Model: {
                    Model* model = AppInternal::getOrLoadModelCached(obj->modelPath);
                    if (!model) break;
                    if (!RHI::valid(m_scenePSO)) break;
                    // La carga PEREZOSA de arriba crea mallas, y Mesh::setupMesh() deja el VAO a 0
                    // → desbindearía el VAO del pipeline JUSTO antes de dibujar (glDrawElements con
                    // VAO 0 = GL_INVALID_OPERATION). Rebindeamos el pipeline después de cargar.
                    // Desaparece cuando la creación de recursos deje de tocar el estado global.
                    sceneCtx->bindPipeline(m_scenePSO);
                    model->drawRHI(*sceneCtx);
                    ++renderedDrawCalls;
                    renderedVertices  += model->getVertexCount();
                    renderedTriangles += model->getTriangleCount();
                    break;
                }
                case Haruka::RenderKind::MeshComponent: {
                    if (!obj->meshRenderer || !obj->meshRenderer->isResident()) break;
                    if (!RHI::valid(m_scenePSO)) break;
                    sceneCtx->bindPipeline(m_scenePSO);   // ver nota del caso Model (estado GL)
                    obj->meshRenderer->renderRHI(*sceneCtx);
                    ++renderedDrawCalls;
                    renderedVertices  += obj->meshRenderer->getResidentVertexCount();
                    renderedTriangles += obj->meshRenderer->getResidentTriangleCount();
                    break;
                }
                case Haruka::RenderKind::Primitive: {
                    SimpleMesh* primitiveMesh = AppInternal::getPrimitiveMesh(command.primitive);
                    if (!primitiveMesh) break;
                    if (!RHI::valid(m_scenePSO)) break;
                    sceneCtx->bindPipeline(m_scenePSO);   // ver nota del caso Model (estado GL)
                    primitiveMesh->drawRHI(*sceneCtx);
                    ++renderedDrawCalls;
                    renderedVertices  += primitiveMesh->getVertexCount();
                    renderedTriangles += primitiveMesh->getTriangleCount();
                    break;
                }
                case Haruka::RenderKind::None:
                    break;
            }
        }
        } // scene.objects.draw

        // Transición: el pase de escena deja bindeados el programa y el VAO del PSO. Los pases
        // siguientes (terreno/agua/islas) siguen en GL directo; dejamos el VAO a 0 para que
        // ninguno pueda MODIFICAR por accidente el VAO del pipeline (p.ej. un glBindBuffer de
        // element array se aplicaría al VAO bindeado y lo corrompería para el frame siguiente).
        if (RHI::valid(m_scenePSO)) sceneCtx->endRenderPass();
        glBindVertexArray(0);

        _iRenderedDrawCalls = renderedDrawCalls;
        _iRenderedVertices  = renderedVertices;
        _iRenderedTriangles = renderedTriangles;
        _iTotalVertices     = renderedVertices;
        _iTotalTriangles    = renderedTriangles;

        // Terrain streaming: update LOD + render planet chunks
        if (_planetarySystem) {
            {
                HARUKA_PROFILE("planetary.update(LOD+stream)");
                _planetarySystem->syncFromScene(*_currentScene);
                _planetarySystem->update(deltaTime > 0.0f ? deltaTime : 0.016, glm::dvec3(_camera->position));
            }

            _iVisibleChunks         = _planetarySystem->getGPUChunkCount();
            _iResidentChunks        = _planetarySystem->getCachedChunks();
            _iPendingChunkLoads     = _planetarySystem->getPendingChunks();
            _iQueuedChunks          = _planetarySystem->getQueuedChunks();
            _iPendingChunkEvictions = 0;
            _iTrackedChunks         = _iVisibleChunks + _iResidentChunks;
            _iResidentMemoryMB      = _planetarySystem->getCacheMemoryMB();
            _iMaxMemoryMB           = _planetarySystem->getCacheMaxMemoryMB();

            // --- SHADOW PASS (sun): the game's casters (props) project depth into a
            // depth map; the terrain receives it. All camera-relative (camera at origin).
            glm::mat4 lightSpace(1.0f);
            bool shadowsOn = false;
            // Read the LIVE setting (config panel + save): 0=Off,1=Low,2=Medium,3=High.
            int sq = (int)Haruka::SettingsManager::get().graphics().shadowQuality;
            if (_camera && _gameInterface && _gameInterface->onRenderShadow && sq > 0) {
                unsigned res = (sq == 1) ? 1024u : (sq == 2) ? 2048u : 4096u; // resolution per quality
                if (!_shadow || _shadow->shadowWidth != res) _shadow = std::make_unique<Shadow>(res, res);
                glm::vec3 sunDir = glm::normalize(frameData.sunDirection); // hacia el sol
                glm::vec3 lup = (std::abs(sunDir.y) < 0.95f) ? glm::vec3(0,1,0) : glm::vec3(1,0,0);
                // TIGHT area around the player → more resolution per metre (sharpness).
                // ±42 m: at 4096 ≈ 2 cm/texel (was ±75 → 3.7 cm, pixels were visible).
                const float D = 90.0f, S = 42.0f;
                glm::mat4 lView = glm::lookAt(sunDir * D, glm::vec3(0.0f), lup); // luz desde el sol
                glm::mat4 lProj = glm::ortho(-S, S, -S, S, 1.0f, 2.0f * D);
                lightSpace = lProj * lView;

                _shadow->bindForWriting();
                glViewport(0, 0, _shadow->shadowWidth, _shadow->shadowHeight);
                // El shadow map usa proyección ORTOGRÁFICA propia (NO invertida) → convención
                // estándar: lejos = 1, test LESS. Hay que fijarlo explícitamente porque el estado
                // global viene del último PSO de la escena, que es reversed-Z (clear 0 / GEQUAL).
                glClearDepth(1.0);
                glDepthFunc(GL_LESS);
                glClear(GL_DEPTH_BUFFER_BIT);
                glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE); glDisable(GL_CULL_FACE);
                _gameInterface->onRenderShadow(lightSpace, glm::vec3(_camera->position));
                shadowsOn = true;
                glBindFramebuffer(GL_FRAMEBUFFER, m_sceneTargetFBO);
                glViewport(0, 0, m_postActive ? (uint32_t)m_postW : width,
                                 m_postActive ? (uint32_t)m_postH : height);
            }

            // Parámetros del PASE de terreno (antes uniforms sueltos del shader; ahora viajan en
            // el UBO TerrainParams del TerrainRenderer, que ata su propio PSO). El estado de
            // rasterizado (depth LEQUAL, cull back, blend off) lo hornea ese pipeline.
            _planetarySystem->setTerrainFog(Haruka::SettingsManager::get().graphics().fog);
            _planetarySystem->setTerrainShadow(
                (shadowsOn && _shadow) ? uboDev->getDepthTexture(_shadow->pass()) : RHI::TextureHandle{},
                lightSpace, shadowsOn && _shadow);

            glFrontFace(GL_CCW);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

            // Bind the scene target (the offscreen post target when active, else
            // the default FBO) in case a previous pass left a custom one bound.
            glBindFramebuffer(GL_FRAMEBUFFER, m_sceneTargetFBO);
            if (m_postActive)
                glViewport(0, 0, (uint32_t)m_postW, (uint32_t)m_postH);

            {
            HARUKA_PROFILE("terrain.draw");
            // UBOs per-frame (0) y per-object (1): los ata el llamador y siguen atados durante
            // todo el pase (los binding points son estado del contexto, no del pipeline) → el
            // PSO del terreno solo ata LO SUYO (SSBO por-draw, TerrainParams, texturas).
            RHI::Context* terrainCtx = uboDev->beginFrame();
            terrainCtx->bindUniformBuffer(0, m_uboPerFrameH);
            terrainCtx->bindUniformBuffer(1, m_uboPerObjectH);
            // Camera-relative view-projection for frustum culling (matches the
            // shader's projection * mat3(view) — view rotation only, no translation).
            const float aspectC = (height > 0u) ? (float)width / (float)height : 1.0f;
            glm::mat4 projC    = _camera->getProjectionMatrix(aspectC);
            glm::mat4 camRelVP = projC * glm::mat4(glm::mat3(_camera->getViewMatrix()));
            _planetarySystem->setTerrainCullMatrix(camRelVP);
            // LOD v3 F1: px por (unidad de mundo / distancia) = altura/(2·tan(fovY/2)).
            // proj[1][1] = 1/tan(fovY/2). Lo usa el split por error en pantalla.
            _planetarySystem->setLODScreenK((double)height * 0.5 * (double)projC[1][1]);
            for (const auto& planet : _planetarySystem->getPlanets()) {
                PerObjectUBOData terrainObj{};
                terrainObj.model                    = glm::mat4(1.0f);
                terrainObj.baseColorAndPlanetRadius = glm::vec4(0.76f, 0.78f, 0.82f, (float)planet.radius);
                // xyz = (camera − planet center) in double→float (precise). The
                // shader reconstructs relPos = FragPos + this → height/slope/climate.
                // w = PERFIL del cuerpo (0 terran · 1 luna · 2 gas) → coloreado por tipo.
                float prof = 0.0f;
                {
                    const auto& cfg = planet.terrainSettings.contains("config")
                                    ? planet.terrainSettings["config"] : planet.terrainSettings;
                    const std::string ps = cfg.value("profile", std::string("terran"));
                    prof = (ps == "moon") ? 1.0f : (ps == "gas") ? 2.0f : 0.0f;
                }
                terrainObj.planetCenterAndFlag      = glm::vec4(
                    glm::vec3(glm::dvec3(_camera->position) - planet.position), prof);

                uboDev->updateBuffer(m_uboPerObjectH, 0, sizeof(PerObjectUBOData), &terrainObj);

                _planetarySystem->renderPlanetTerrain(planet.name, glm::dvec3(_camera->position));
            }
            }


            // --- Floating islands pass (opaque, after terrain) ---
            // Reusan los shaders del planeta pero con su PROPIO PSO (layout de 2 streams vec3, sin
            // culling) y su SSBO de 1 elemento (morph = 0). El UBO per-object sigue siendo el del
            // ÚLTIMO planeta del bucle de arriba — mismo comportamiento que antes.
            {
                HARUKA_PROFILE("islands.draw");
                for (const auto& planet : _planetarySystem->getPlanets())
                    _planetarySystem->renderPlanetIslands(planet.name, glm::dvec3(_camera->position));
            }

            // (Resources —trees/rocks/minerals— are drawn by the GAME in onRenderWorld;
            //  they are game content, not engine content.)

            // --- Water pass (ocean shell, after terrain) ---
            // El estado (depth LEQUAL sin escritura, blend alfa, a dos caras) y el programa los
            // hornea el PSO del WaterRenderer. Aquí solo se calculan sus PARÁMETROS de pase, que
            // antes eran ~15 glUniform sueltos y ahora viajan en el UBO WaterParams.
            // DEBUG: volcado del target de escena ANTES del agua (para comparar con el de después).
            if (const char* pre = getenv("HARUKA_SCENE_SHOT_PRE")) {
                static bool tk = false;
                const int secs = getenv("HARUKA_SHOT_SEC") ? atoi(getenv("HARUKA_SHOT_SEC")) : 60;
                if (!tk && SDL_GetTicks() > (uint64_t)secs * 1000) {
                    tk = true;
                    const int sw = m_postActive ? (int)m_postW : (int)width;
                    const int sh = m_postActive ? (int)m_postH : (int)height;
                    std::vector<unsigned char> px((size_t)sw * sh * 3);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_sceneTargetFBO);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glReadPixels(0, 0, sw, sh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
                    if (FILE* f = fopen(pre, "wb")) {
                        fprintf(f, "P6\n%d %d\n255\n", sw, sh);
                        for (int y = sh - 1; y >= 0; --y) fwrite(&px[(size_t)y * sw * 3], 1, (size_t)sw * 3, f);
                        fclose(f);
                        fprintf(stderr, "[SCENE-PRE] escrito %s\n", pre);
                    }
                    glBindFramebuffer(GL_FRAMEBUFFER, m_sceneTargetFBO);
                }
            }

            if (!getenv("HARUKA_NO_WATER")) {   // DEBUG TEMPORAL: aislar el pase de agua
                HARUKA_PROFILE("water.draw");
                WaterRenderer::PassParams wp;
                wp.time = (float)(SDL_GetTicks() / 1000.0);

                // (F1) COPIA de la profundidad de la escena → el agua la muestrea para saber cuánta
                // agua atraviesa el rayo. Antes re-derivaba el terreno per-píxel (~150 líneas de
                // Perlin duplicadas del generador); con reversed-Z el depth ya lo dice. Se copia
                // (no se lee el depth de la escena directamente) porque muestrear una textura ATADA
                // al FBO activo es un feedback loop = comportamiento indefinido.
                {
                    const int dw = m_postActive ? (int)m_postW : (int)width;
                    const int dh = m_postActive ? (int)m_postH : (int)height;
                    if (!RHI::valid(m_sceneDepthCopy) || m_depthCopyW != dw || m_depthCopyH != dh) {
                        if (RHI::valid(m_sceneDepthCopy)) uboDev->destroy(m_sceneDepthCopy);
                        RHI::RenderTargetDesc dd;
                        dd.width = (uint32_t)dw; dd.height = (uint32_t)dh;
                        dd.colorFormats  = {};                       // solo profundidad
                        dd.hasDepth      = true;
                        dd.depthFormat   = RHI::Format::D32F;        // igual que el target de escena
                        dd.depthAsTexture = true;                    // muestreable por el shader
                        dd.depthFilter   = RHI::Filter::Nearest;
                        m_sceneDepthCopy = uboDev->createRenderTarget(dd);
                        m_depthCopyW = dw; m_depthCopyH = dh;
                    }
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_sceneTargetFBO);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, uboDev->nativeFramebuffer(m_sceneDepthCopy));
                    glBlitFramebuffer(0, 0, dw, dh, 0, 0, dw, dh, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
                    glBindFramebuffer(GL_FRAMEBUFFER, m_sceneTargetFBO);
                    wp.sceneDepth = uboDev->getDepthTexture(m_sceneDepthCopy);
                    wp.nearPlane  = _camera->getNearPlane();
                }

                const glm::dvec3 camD = glm::dvec3(_camera->position);
                for (const auto& planet : _planetarySystem->getPlanets()) {
                    glm::dvec3 up = camD - planet.position;
                    double upLen = glm::length(up);
                    const double waterAlt = (planet.radius > 1e-9)
                        ? (upLen - planet.radius) / planet.radius : 0.0;

                    // Solo planetas con OCÉANO (perfil "terran"). Júpiter/gas → sin agua.
                    const auto& wcfg = planet.terrainSettings.contains("config")
                        ? planet.terrainSettings["config"] : planet.terrainSettings;
                    const bool planetHasOcean = wcfg.value("profile", std::string("terran")) == "terran";
                    if (!planetHasOcean) continue;

                    // LOD v3 F4: FUERA la esfera de océano. Con F1 (error en pantalla) el agua por
                    // chunks se ve fina a TODA distancia (costa exacta, sub-píxel lejos) → la esfera
                    // ("pegote" + discard analítico de continentes) era un camuflaje innecesario y
                    // ya no se dibuja. El agua es SOLO chunks (honesta, sigue la forma del terreno).

                    // --- AGUA por chunks (oleaje Gerstner + físicas) a TODA distancia ---
                    // (WaterRenderer salta los chunks lejos del jugador → esos los cubre la esfera).
                    up = (upLen > 1e-9) ? up / upLen : glm::dvec3(0.0, 1.0, 0.0);
                    glm::dvec3 ref = (std::abs(up.y) < 0.99) ? glm::dvec3(0,1,0) : glm::dvec3(1,0,0);
                    glm::dvec3 T = glm::normalize(glm::cross(ref, up));
                    glm::dvec3 B = glm::cross(up, T);

                    wp.waveUp        = glm::vec3(up);
                    wp.waveTangent   = glm::vec3(T);
                    wp.waveBitangent = glm::vec3(B);

                    // (F1) Aquí se pasaban los params del generador (seed, seaThreshold, coastWidth…)
                    // para que el fragment del agua RE-DERIVARA el terreno y recortara la costa.
                    // Ya no: el depth buffer da la profundidad real → costa exacta y gratis.
                    // Lo único que queda es un ANCLA de mundo para el ruido de la espuma per-píxel.
                    wp.planetRelCam = glm::vec3(camD - planet.position);
                    // (F3) El CAMPO del planeta → el agua sabe DÓNDE HAY AGUA (sin esto la lámina
                    // del océano existe también bajo la tierra y se ve al cavar).
                    {
                        const auto& wcfg2 = planet.terrainSettings.contains("config")
                            ? planet.terrainSettings["config"] : planet.terrainSettings;
                        const uint32_t pseed = (uint32_t)wcfg2.value("seed", 42);
                        int fres = 0;
                        wp.fieldSSBO = Haruka::planetFieldsSSBO(pseed, fres);
                        wp.fieldRes  = fres;
                    }

                    // Oleaje = MAREA (alineación Sol–Luna) × VIENTO atmosférico (ráfagas):
                    // marea viva + ráfaga = mar picado; marea muerta sin viento = calmo.
                    // CORRIENTE (F5.4): u_windDir = dirección del viento/corriente proyectada al
                    // plano tangente → el oleaje VIAJA en esa dirección (flujo visible). Si está
                    // en calma, una deriva lenta para que el mar nunca se vea estático.
                    glm::vec2 curDir(1.0f, 0.0f);
                    if (_worldSystem) {
                        glm::dvec3 w = glm::dvec3(_worldSystem->getWind(camD));
                        glm::vec2 cd((float)glm::dot(w, T), (float)glm::dot(w, B));
                        float cl = glm::length(cd);
                        if (cl > 1e-3f) curDir = cd / cl;
                        else { float a = (float)(SDL_GetTicks() / 1000.0) * 0.02f; curDir = glm::vec2(cosf(a), sinf(a)); }
                    }
                    wp.windDir = curDir;  // corriente, plano tangente
                    const float tide = _worldSystem ? _worldSystem->getTideFactor() : 1.0f;
                    float windF = 1.0f;
                    if (_worldSystem) {
                        float ws = glm::length(_worldSystem->getWind(camD));
                        windF = glm::clamp(0.85f + ws * 0.10f, 0.8f, 1.8f);
                    }
                    wp.windStrength = tide * windF;   // marea × viento
                    // NIVEL de marea (m): el mar sube/baja siguiendo a la Luna → "respira".
                    wp.tideHeight = _worldSystem ? _worldSystem->getTideHeight(camD) : 0.0f;
                    // Reflejo de cielo solo cerca (más caro); lejos solo especular.
                    wp.waterQuality = (waterAlt < 0.05) ? 1.0f : 0.0f;

                    _planetarySystem->setWaterPassParams(wp);
                    _planetarySystem->renderPlanetWater(planet.name, camD);
                }
                // El PSO del agua deja blend ON y depth-write OFF. Los pases que vienen después
                // (onRenderWorld del JUEGO) siguen siendo GL crudo y dan por hecho el estado
                // normal → hay que restaurarlo a mano. Cuando el juego también use PSOs, sobra.
                glDisable(GL_BLEND);
                glDepthMask(GL_TRUE);
            }

            // DEBUG TEMPORAL: HARUKA_SCENE_SHOT=<ruta.ppm> vuelca el TARGET DE ESCENA (terreno+agua,
            // sin la UI del juego encima) tras HARUKA_SHOT_SEC segundos. Para inspeccionar el render
            // aunque la pantalla de carga tape el backbuffer.
            if (const char* shot = getenv("HARUKA_SCENE_SHOT")) {
                static bool taken = false;
                const int secs = getenv("HARUKA_SHOT_SEC") ? atoi(getenv("HARUKA_SHOT_SEC")) : 60;
                if (!taken && SDL_GetTicks() > (uint64_t)secs * 1000) {
                    taken = true;
                    const int sw = m_postActive ? (int)m_postW : (int)width;
                    const int sh = m_postActive ? (int)m_postH : (int)height;
                    std::vector<unsigned char> px((size_t)sw * sh * 3);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_sceneTargetFBO);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glReadPixels(0, 0, sw, sh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
                    if (FILE* f = fopen(shot, "wb")) {
                        fprintf(f, "P6\n%d %d\n255\n", sw, sh);
                        for (int y = sh - 1; y >= 0; --y) fwrite(&px[(size_t)y * sw * 3], 1, (size_t)sw * 3, f);
                        fclose(f);
                        fprintf(stderr, "[SCENE-SHOT] escrito %s (%dx%d)\n", shot, sw, sh);
                    }
                    // ¿ESCRIBE PROFUNDIDAD el terreno? Volcamos el depth del target de escena.
                    std::vector<float> dz((size_t)sw * sh);
                    glReadPixels(0, 0, sw, sh, GL_DEPTH_COMPONENT, GL_FLOAT, dz.data());
                    double mn = 2.0, mx = -1.0; size_t nFar = 0;
                    for (float d : dz) { mn = std::min(mn, (double)d); mx = std::max(mx, (double)d);
                                         if (d >= 0.999999f) ++nFar; }
                    fprintf(stderr, "[DEPTH] min=%.6f max=%.6f  px_al_fondo(=1.0)=%.1f%%\n",
                            mn, mx, 100.0 * (double)nFar / (double)dz.size());
                    glBindFramebuffer(GL_FRAMEBUFFER, m_sceneTargetFBO);
                }
            }

            // Restore main shader for subsequent rendering
            _mainShader->use();

            // Add terrain draw stats on top of regular object stats
            const auto ts = _planetarySystem->getTerrainDrawStats();
            _iRenderedDrawCalls += ts.draws;
            _iRenderedVertices  += ts.vertices;
            _iRenderedTriangles += ts.triangles;
            _iTotalDrawCalls    = _iRenderedDrawCalls;
            _iTotalVertices     = _iRenderedVertices;
            _iTotalTriangles    = _iRenderedTriangles;
        }
    }

    if (_gameInterface && _gameInterface->onRenderWorld && _camera) {
        const float      aspect  = (height > 0u) ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
        const glm::mat4  view    = _camera->getViewMatrix();
        const glm::mat4  proj    = _camera->getProjectionMatrix(aspect);
        const glm::vec3  camPos  = glm::vec3(_camera->position);
        _gameInterface->onRenderWorld(view, proj, camPos);
    }

    // --- Post-processing composite -----------------------------------------
    // Resolve the offscreen scene target to the screen: upscales the render-scaled
    // image and applies the present-time effects (FXAA, and later bloom). With
    // FXAA off and scale 1.0 the scene never took this path (m_postActive false).
    // === PASE 2 MIGRADO A PSO/Context: el present/composite (FXAA + bloom + upscale). ===
    if (m_postActive && _postScene) {
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
            if (wantBloom) bloomTex = renderBloom(_postScene->getColorTextureHandle());
            const bool haveBloom = wantBloom && RHI::valid(bloomTex);

            PresentParams p{};
            p.texel[0]      = 1.0f / (float)m_postW;
            p.texel[1]      = 1.0f / (float)m_postH;
            p.bloomStrength = gpost.bloomStrength;
            p.fxaa          = wantFXAA  ? 1.0f : 0.0f;
            p.bloom         = haveBloom ? 1.0f : 0.0f;
            dev->updateBuffer(m_presentUBO, 0, sizeof(p), &p);

            RHI::Context* ctx = dev->beginFrame();
            RHI::ClearValues keep;                       // el quad cubre la pantalla entera
            keep.clearColor = false; keep.clearDepth = false;

            ctx->beginRenderPass(RHI::RenderPassHandle{}, keep);  // id 0 = backbuffer (pantalla)
            ctx->setViewport(0, 0, (int)width, (int)height);      // el pass a pantalla NO fija viewport
            ctx->bindPipeline(m_presentPSO);
            ctx->bindVertexBuffer(m_quadBuf);
            ctx->bindUniformBuffer(3, m_presentUBO);
            ctx->bindTexture(0, _postScene->getColorTextureHandle());
            // El sampler u_bloomTex se lee SIEMPRE en el shader aunque u_bloom sea 0; bindea algo
            // válido para no dejar la unidad 1 con basura de un pase anterior.
            ctx->bindTexture(1, haveBloom ? bloomTex : _postScene->getColorTextureHandle());
            ctx->draw(4);
            ctx->endRenderPass();
        }

        // Transición: ImGui y otros pases posteriores siguen en GL directo y esperan este estado.
        glBindVertexArray(0);
        glActiveTexture(GL_TEXTURE0); // leave unit 0 active for following passes
        glEnable(GL_DEPTH_TEST);
    }

    // Clean screenshot (no HUD): capture here, after the 3D scene is composited to
    // the screen but BEFORE ImGui draws over it.
    if (!_editorTarget) captureScreenshotIfPending((int)width, (int)height);

    if (_imguiCallback) {
        _imguiCallback();
    }

    if (_editorTarget) {
        _editorTarget->unbind();
    }

    // GPU timer end — must match the glBeginQuery at the top of this function.
    glEndQuery(GL_TIME_ELAPSED);
    m_gpuTimerIssued[m_gpuTimerFrame] = true;
    m_gpuTimerFrame ^= 1;
}

void Application::renderFrame() {
    auto now = std::chrono::high_resolution_clock::now();
    const std::chrono::duration<float> elapsed = now - _frameStart;
    _frameStart = now;

    deltaTime = elapsed.count();
    _lastFrameTimeMs = deltaTime * 1000.0f;

#ifdef HARUKA_NETWORK
    if (_currentScene) {
        for (const auto& transfer : m_dgs.pollEntities()) {
            auto obj = std::make_shared<Haruka::SceneObject>();
            obj->name = "entity_" + std::to_string(transfer.uuid);
            obj->type = (transfer.type == DGS::ENT_PLAYER) ? "Character" :
                        (transfer.type == DGS::ENT_NPC)    ? "Character" : "Spacecraft";
            obj->position = Haruka::WorldPos(
                transfer.chunkX * Haruka::Units::KM + transfer.pos[0],
                transfer.chunkY * Haruka::Units::KM + transfer.pos[1],
                transfer.chunkZ * Haruka::Units::KM + transfer.pos[2]
            );
            _currentScene->addLoadedObject(obj);
        }
    }
#endif

    // In standalone mode (run()), the main loop handles swap + FPS.
    // In editor mode (no _window), renderFrame() is called externally
    // and the editor manages the swap.
    buildRenderQueue();
    renderFrameContent();

    if (!_window) return; // editor path — caller handles swap

    _window->swapBuffers();

    _fpsFrameCount++;
    _fpsLastTime += deltaTime;
    if (_fpsLastTime >= 1.0) {
        _lastFps      = static_cast<float>(_fpsFrameCount / _fpsLastTime);
        _fpsFrameCount = 0;
        _fpsLastTime   = 0.0;
    }
}

void Application::requestScreenshot(const std::string& path) {
    m_screenshotPath    = path;
    m_screenshotPending = true;
}

void Application::captureScreenshotIfPending(int width, int height) {
    if (!m_screenshotPending || width <= 0 || height <= 0) return;
    m_screenshotPending = false;

    // Read the composited back buffer (RGBA8), then flip rows (GL is bottom-up).
    std::vector<unsigned char> buf((size_t)width * height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());

    std::vector<unsigned char> flipped((size_t)width * height * 4);
    const size_t stride = (size_t)width * 4;
    for (int y = 0; y < height; ++y)
        std::memcpy(&flipped[(size_t)y * stride], &buf[(size_t)(height - 1 - y) * stride], stride);

    std::string path = m_screenshotPath;
    if (path.empty()) {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char name[64];
        std::strftime(name, sizeof(name), "screenshots/shot_%Y%m%d_%H%M%S.png", &tm);
        path = name;
    }

    if (Haruka::writePNG(path, width, height, 4, flipped.data()))
        std::cout << "[Application] Screenshot saved: " << path << std::endl;
    else
        std::cerr << "[Application] Screenshot FAILED: " << path << std::endl;
}


}} // namespace Haruka::Core
