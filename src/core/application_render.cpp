// Application — render pipeline.
// The per-frame work: build the render queue, the deferred/forward object pass,
// terrain/islands/water passes, the standalone post-processing composite and the
// GPU timer. Lifecycle/orchestration lives in application.cpp; the GL asset
// caches in application_assets.cpp.

#include <chrono>
#include <cmath>
#include "application.h"
#include "application_internal.h"
#include "rhi/rhi_context.h"   // ruta PSO: comandos de dibujo del frame (bloom migrado)

#include <algorithm>

#include "core/logger.h"

#include <SDL3/SDL.h>
#include <glm/glm.hpp>

#include "game/planetary_system.h"
#include "core/components/mesh_renderer_component.h"
#include "core/components/material_component.h"
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
    // MATERIAL del objeto (MaterialComponent). Va al FINAL a propósito: planet.frag y preview.*
    // declaran su propio bloque con los tres campos de arriba y siguen leyendo los mismos
    // offsets; solo los shaders que quieran material declaran el bloque completo.
    glm::vec4 materialPBR;              // x=metallic y=roughness z=ao w=máscara de texturas (bits)
    glm::vec4 materialEmission;         // rgb=emisión, a=libre
};
static_assert(sizeof(PerObjectUBOData) == 128, "PerObjectUBOData std140 size mismatch");

// Bits de `materialPBR.w`: qué slots de textura están REALMENTE atados este draw. El shader no
// puede preguntárselo a un sampler (uno sin atar lee negro, que es un valor válido), así que la
// presencia viaja explícita. Los valores son potencias de 2 → w es exacto en float hasta 2^24.
enum MaterialTexBit {
    kTexAlbedo    = 1,
    kTexNormal    = 2,
    kTexMetallic  = 4,
    kTexRoughness = 8,
    kTexAO        = 16,
};

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

// UBO del pase de LLUVIA (binding 5). x=tiempo · y=lluvia[0,1] · z=aspecto · w=inclinación(viento).
struct RainParams { glm::vec4 p; };
static_assert(sizeof(RainParams) == 16, "RainParams std140 size mismatch");

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
    float     time;           // 108..112  (segundos; para el MOVIMIENTO de las nubes)
    glm::vec4 weather;        // 112..128  x=humedad[0,1] · y=tempC · z=precipitación[0,1] · w=COBERTURA de nube[0,1]
    // El VIENTO del clima, en el plano tangente del observador (este/norte). Antes las nubes derivaban
    // con una constante hardcodeada (0.012, 0.006) y el follaje con su propio reloj → cada cosa soplaba
    // por su lado. Un solo vector para todos. w = 1 si lo que cae es NIEVE (nube gris ≠ nube de nieve).
    glm::vec4 wind;           // 128..144  x=este(m/s) · y=norte(m/s) · z=racha · w=esNieve
};
static_assert(sizeof(SkyParams) == 144, "SkyParams std140 size mismatch");
} // namespace

void Application::setupQuad() {
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

void Application::buildRenderQueue() {
    HARUKA_PROFILE("buildRenderQueue");

    if (!_currentScene) { g_sceneRenderQueue.clear(); return; }

    // Cache the STATIC scene classification (kind/lod per object), which only
    // changes when the object set changes — not every frame. The expensive part
    // is the per-object string lowercasing + substring matching; transforms are
    // read live from the shared_ptr at draw time, so movement still updates.
    const auto& objects = _currentScene->getAllObjects();
    if (m_renderQueueDirty || objects.size() != m_renderQueueObjCount) {
        // INCREMENTAL: solo se clasifican los objetos NUEVOS. Clasificar es la parte cara (comparar
        // cadenas de tipo, mirar el mapa de propiedades) y antes se rehacía la escena ENTERA cada vez
        // que cambiaba el nº de objetos — con escombros apareciendo y caducando, eso es a todas horas.
        // Ahora un derrumbe cuesta clasificar sus 48 piezas, no reclasificar la ciudad.
        // Se identifica por UID, no por puntero: una dirección liberada se reutiliza para el objeto
        // siguiente, y entonces se daría por "ya clasificado" algo nuevo. Además así la fase de borrado
        // NO desreferencia punteros que pueden estar colgando.
        m_presentObjects.clear();
        m_presentObjects.reserve(objects.size());
        for (const auto& o : objects) if (o) m_presentObjects.insert(o->uid);

        // 1) Fuera los que ya no están en la escena (intercambio con el último → sin desplazar nada).
        if (m_staticQueueCount > g_sceneRenderQueue.size()) m_staticQueueCount = g_sceneRenderQueue.size();
        for (std::size_t k = 0; k < m_staticQueueCount; ) {
            if (m_presentObjects.count(g_sceneRenderQueue[k].uid)) { ++k; continue; }
            m_queuedObjects.erase(g_sceneRenderQueue[k].uid);
            g_sceneRenderQueue[k] = g_sceneRenderQueue[m_staticQueueCount - 1];
            --m_staticQueueCount;
        }
        g_sceneRenderQueue.resize(m_staticQueueCount);

        // 2) Entran los nuevos (los únicos que se clasifican).
        for (const auto& o : objects) {
            if (!o || m_queuedObjects.count(o->uid)) continue;
            Haruka::RenderCommand c = Haruka::classifySceneObject(*o);
            if (c.kind != Haruka::RenderKind::None) g_sceneRenderQueue.push_back(c);
            m_queuedObjects.insert(o->uid);    // se marca aunque no se dibuje: no reclasificarlo cada vez
        }
        m_staticQueueCount    = g_sceneRenderQueue.size();
        m_renderQueueObjCount = objects.size();
        m_renderQueueDirty    = false;
    } else {
        // Quita solo lo que se añadió el frame anterior (fantasmas de red). Sin copias ni realloc.
        g_sceneRenderQueue.resize(m_staticQueueCount);
    }

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

        m_bloomUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(BloomParams), nullptr,
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

    return m_bloomTexH[0];   // handle: el composite (también PSO) lo bindea por el Context
}

void Application::renderFrameContent() {
    // Editor path calls this externally (no standalone loop) → reset here.
    // Standalone loop resets at its own frame boundary so game.onUpdate is measured.
    if (_editorTarget) Haruka::Profiler::get().newFrame();
    HARUKA_PROFILE("renderFrameContent");
    const uint32_t width  = _window ? _window->getWidth()  : static_cast<uint32_t>(m_editorViewportW);
    const uint32_t height = _window ? _window->getHeight() : static_cast<uint32_t>(m_editorViewportH);

    // GPU timer disabled during RHI migration

    // --- Standalone post-processing target ---------------------------------
    // Route the 3D scene through an offscreen HDR target when any screen-space
    // effect or non-1.0 render scale is active. With everything off this stays
    // false → the scene renders straight to the screen exactly as before.
    const auto& gpost  = Haruka::SettingsManager::get().graphics();
    const float rscale = std::min(std::max(gpost.renderScale, 0.5f), 2.0f);
    const bool  wantFXAA = (gpost.antialiasing == Haruka::Settings::AntialiasingMode::FXAA);
    const bool  wantBloom = gpost.bloom;
    const int   renderW  = std::max(1, (int)(width  * rscale));
    const int   renderH  = std::max(1, (int)(height * rscale));
    m_postActive = !_editorTarget && (rscale != 1.0f || wantFXAA || wantBloom);
    if (m_postActive) {
        if (!_postScene || m_postW != renderW || m_postH != renderH) {
            _postScene = std::make_unique<HDR>((unsigned)renderW, (unsigned)renderH);
            m_postW = renderW; m_postH = renderH;
        }
    }
    // Mecánica celeste: el WorldSystem usa el planeta REAL (su PlanetarySystem propio
    // está vacío). Le pasamos centro/radio del planeta activo y avanzamos día/noche +
    // luna + marea + viento cada frame (WorldSystem::update no se llama).
    if (_worldSystem && _planetarySystem) {
        glm::dvec3 pc; double pr;
        if (_planetarySystem->getActivePlanet(pc, pr))
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
    RHI::Device* frameDev = RHI::device();
    RHI::Context* frameCtx = frameDev ? frameDev->beginFrame() : nullptr;
    RHI::RenderPassHandle sceneTargetPass;
    if (_editorTarget) {
        sceneTargetPass = _editorTarget->getPass();
    } else if (m_postActive && _postScene) {
        sceneTargetPass = _postScene->getPass();
    }

    // CLEAR DEL FRAME, incondicional y en UN SOLO SITIO. Antes lo hacía el pase de cielo, que vive
    // bajo tres condiciones (`_worldSystem && _camera && _planetarySystem`, que haya planeta ACTIVO
    // y que el PSO del cielo compilara). Una escena sin planeta —la típica del editor: unas
    // primitivas y una luz— no cumplía la segunda, así que no se limpiaba NI color NI profundidad:
    // con reversed-Z el test es GREATER, el z-buffer conservaba el del frame anterior y a partir
    // del segundo frame ningún fragmento a la misma profundidad volvía a pasar → viewport negro.
    // El cielo, cuando corre, reabre este mismo pase SIN clear y pinta encima.
    if (frameCtx) {
        RHI::ClearValues frameClear;
        frameClear.clearColor = true;
        frameClear.color[0] = sky.r; frameClear.color[1] = sky.g; frameClear.color[2] = sky.b;
        frameClear.color[3] = 1.0f;
        frameClear.clearDepth = true; frameClear.depth = 0.0f;   // reversed-Z: lejos = 0
        frameCtx->beginRenderPass(sceneTargetPass, frameClear);
        if (!RHI::valid(sceneTargetPass))            // el pass a pantalla NO fija viewport
            frameCtx->setViewport(0, 0, (int)width, (int)height);
        frameCtx->endRenderPass();
    }

    // Pase de cielo procedural (gradiente + sol + estrellas) como FONDO: triángulo
    // fullscreen SIN escribir profundidad → el terreno/objetos se pintan encima.
    if (_worldSystem && _camera && _planetarySystem) {
        glm::dvec3 pc; double pr;
        if (_planetarySystem->getActivePlanet(pc, pr)) {
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

            static const auto s_skyT0 = std::chrono::steady_clock::now();
            const float skyTime = std::chrono::duration<float>(std::chrono::steady_clock::now() - s_skyT0).count();

            // ── CLIMA: se LEE del mundo, no se inventa aquí ─────────────────────────────────────
            // Antes este bloque calculaba la lluvia con `0.5+0.5·sin(t·0.05)` mientras sky.frag
            // calculaba SUS nubes con `fbm(t·0.008)`. Dos fórmulas sin relación → llovía con el
            // cielo despejado. Ahora ambos leen el MISMO `WeatherSample`: la cobertura que el shader
            // dibuja es la que produjo la lluvia, y la precipitación existe solo bajo esa nube.
            Haruka::WeatherSample wx;
            if (_planetarySystem) wx = _planetarySystem->weatherAt(camD);
            float precip = wx.precip;               // lluvia O nieve: las dos encapotan el cielo
            float rain   = wx.rainAmount();         // solo LLUVIA: es lo que dibuja el pase de gotas
            float cover  = wx.cloudCover;
            if (m_rainOverride >= 0.0f) {           // consola `rain` (pruebas): fuerza el temporal
                rain   = m_rainOverride;            // ...y CON él la nube que lo justifica, o volvería
                precip = m_rainOverride;            //    a verse lluvia con cielo azul (el bug de antes)
                cover  = glm::max(cover, rain);
            }
            m_rainAmount = rain;                                    // gotas
            m_snowAmount = (m_rainOverride >= 0.0f) ? 0.0f : wx.snowAmount();   // copos
            m_windVec    = wx.wind;                                 // el ÚNICO viento del mundo

            // MOJADO DEL SUELO: se INTEGRA, no se copia de la lluvia. Mojarse cuesta ~30 s de
            // chaparrón; secarse, minutos. Esa asimetría es la que hace que el suelo siga oscuro
            // (y brillante) un rato después de escampar, en vez de apagarse con la última gota.
            {
                static auto s_wetT = std::chrono::steady_clock::now();
                const auto  now    = std::chrono::steady_clock::now();
                const float dtW    = glm::clamp(std::chrono::duration<float>(now - s_wetT).count(), 0.0f, 0.25f);
                s_wetT = now;
                m_groundWetness += dtW * (rain > 0.01f ? rain / 30.0f : -1.0f / 240.0f);
                m_groundWetness  = glm::clamp(m_groundWetness, 0.0f, 1.0f);

                // NIEVE ACUMULADA: cuaja mientras nieva y FUNDE con la temperatura del sitio. No es
                // "hace frío ⇒ suelo blanco": si no ha nevado no hay nieve, y si sube la temperatura
                // se va aunque siga siendo invierno. Fundir es MUCHO más lento que cuajar.
                const float melt = glm::smoothstep(0.0f, 6.0f, wx.tempC) / 420.0f;
                m_snowAccum += dtW * (m_snowAmount > 0.01f ? m_snowAmount / 45.0f : -melt);
                m_snowAccum  = glm::clamp(m_snowAccum, 0.0f, 1.0f);
            }

            // Viento del clima proyectado al marco del observador (este/norte) → nubes y lluvia
            // inclinan HACIA EL MISMO LADO. `wind` viene en el marco local del planeta.
            glm::vec3 eastW = glm::normalize(glm::cross(glm::vec3(0, 1, 0), glm::vec3(up)));
            if (!std::isfinite(eastW.x)) eastW = glm::vec3(1, 0, 0);
            const glm::vec3 northW = glm::cross(glm::vec3(up), eastW);
            const glm::vec2 windEN(glm::dot(wx.wind, eastW), glm::dot(wx.wind, northW));
            m_windSlant = glm::clamp(windEN.x * 0.02f, -0.45f, 0.45f);   // para el pase de lluvia

            SkyParams sp{};
            sp.invViewProjRot = invVPRot;
            sp.sunDir         = sunDir;
            sp.sunElev        = sunElev;
            sp.up             = glm::vec3(up);
            sp.atmo           = atmo;
            sp.sunColor       = sunCol;
            sp.time           = skyTime;   // para el desplazamiento de las nubes
            sp.weather        = glm::vec4(wx.humidity, wx.tempC, precip, cover);
            sp.wind           = glm::vec4(windEN.x, windEN.y, glm::length(wx.wind),
                                          wx.isSnow() ? 1.0f : 0.0f);
            skyDev->updateBuffer(m_skyUBO, 0, sizeof(sp), &sp);

            RHI::Context* ctx = skyDev->beginFrame();
            // SIN clear: el frame ya se limpió arriba (color de cielo + depth), y este triángulo
            // fullscreen cubre la pantalla entera de todas formas.
            // sceneTargetPass ya resuelve _editorTarget (viewport del IDE) o _postScene; si
            // ninguno está activo queda inválido = backbuffer de pantalla. Usarlo aquí es lo
            // que hace que la escena caiga DENTRO del RenderTarget del editor.
            RHI::ClearValues sceneClear;
            sceneClear.clearColor = false; sceneClear.clearDepth = false;
            ctx->beginRenderPass(sceneTargetPass, sceneClear);
            if (!RHI::valid(sceneTargetPass))                        // el pass a pantalla NO fija viewport
                ctx->setViewport(0, 0, (int)width, (int)height);
            ctx->bindPipeline(m_skyPSO);                             // programa + depth off (no escribe z)
            ctx->bindUniformBuffer(5, m_skyUBO);
            ctx->draw(3);                                            // sin vertex buffer: gl_VertexID
            ctx->endRenderPass();

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
        if (!RHI::valid(m_uboPerFrameH)) {
            m_uboPerFrameH = uboDev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PerFrameUBOData), nullptr, RHI::BufferMemory::Dynamic);
        }
        if (!RHI::valid(m_uboPerObjectH)) {
            m_uboPerObjectH = uboDev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PerObjectUBOData), nullptr, RHI::BufferMemory::Dynamic);
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
            sceneCtx->beginRenderPass(sceneTargetPass, keep);
            if (!RHI::valid(sceneTargetPass))           // el pass a pantalla NO fija viewport
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
        // El terreno del planeta usa la MISMA luz que el cielo: sin esto el SimplePlanet
        // iluminaba con una dirección fija y no respondía al sol que se ve en el cielo.
        if (_planetarySystem)
            _planetarySystem->setSunLight(frameData.sunDirection, frameData.sunLightColor,
                                          frameData.ambientStrength);
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

        // Cull face handled by the pipeline

        int renderedDrawCalls = 0;
        int renderedVertices  = 0;
        int renderedTriangles = 0;

        // Piezas de CONSTRUCCIÓN (prop "construction"): en vez de 1 draw por pieza, se acumulan por
        // MODELO y se dibujan INSTANCIADAS tras el bucle (1 draw por modelo). Ver el pase de abajo.
        std::unordered_map<std::string, std::vector<Haruka::InstanceDataFloat>> constInstances;
        // Lo mismo para las PRIMITIVAS de edificio (prop "building"): un edificio tejido son cientos o
        // miles de cubos —las tablas/sillares del muro—, y por-objeto serían cientos o miles de draws.
        // Se agrupan por primitiva → 1 draw por primitiva (por lote, ver la cota de instancias).
        std::unordered_map<int, std::vector<Haruka::InstanceDataFloat>> primInstances;

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

            // Pieza de construcción → al pase INSTANCIADO (no draw individual). Mismo model/color que
            // llevaría por-objeto, así se ve idéntica; el cull de arriba ya se aplicó.
            if (command.instanced && command.kind == Haruka::RenderKind::Model && !obj->modelPath.empty()) {
                Haruka::InstanceDataFloat inst;
                inst.model = AppInternal::getTransformMatrix(*obj, _camera->position);
                inst.color = glm::vec4(baseColor, 1.0f);
                inst.scale = glm::vec3(obj->scale);
                constInstances[obj->modelPath].push_back(inst);
                continue;
            }
            // Primitiva de construcción/edificio → al mismo pase instanciado, agrupada por primitiva.
            if (command.instanced && command.kind == Haruka::RenderKind::Primitive) {
                Haruka::InstanceDataFloat inst;
                inst.model = AppInternal::getTransformMatrix(*obj, _camera->position);
                inst.color = glm::vec4(baseColor, 1.0f);
                inst.scale = glm::vec3(obj->scale);
                primInstances[(int)command.primitive].push_back(inst);
                continue;
            }

            PerObjectUBOData objData{};
            objData.model                    = AppInternal::getTransformMatrix(*obj, _camera->position);
            objData.baseColorAndPlanetRadius = glm::vec4(baseColor, 1.0f);
            // w = 0: normal, 1: procedural terrain, 2: emissive star (no diffuse shading)
            objData.planetCenterAndFlag      = glm::vec4(0.0f, 0.0f, 0.0f, isStar ? 2.0f : 0.0f);
            // Sin MaterialComponent: dieléctrico mate sin texturas → el aspecto de siempre.
            objData.materialPBR              = glm::vec4(0.0f, 0.5f, 1.0f, 0.0f);
            objData.materialEmission         = glm::vec4(0.0f);

            // --- MATERIAL del objeto (albedo/normal/metallic/roughness/ao) -------------------
            // Los slots del MaterialComponent son los MISMOS que hornea el editor de node graph
            // del IDE, así que un grafo bakeado se ve aquí sin más paso intermedio.
            if (obj->material) {
                const Haruka::MaterialComponent& mat = *obj->material;
                // El color del material TIÑE el del objeto: un material no debe borrar el tinte
                // por objeto (dos props del mismo material se distinguen por su color).
                objData.baseColorAndPlanetRadius = glm::vec4(baseColor * mat.albedo, 1.0f);
                objData.materialEmission         = glm::vec4(mat.emission, 0.0f);

                int texMask = 0;
                auto bindSlot = [&](const char* key, uint32_t unit, int bit) {
                    auto it = mat.textures.find(key);
                    if (it == mat.textures.end() || it->second.empty()) return;
                    RHI::TextureHandle tex = AppInternal::getOrLoadMaterialTexture(it->second);
                    if (!RHI::valid(tex)) return;
                    sceneCtx->bindTexture(unit, tex);
                    texMask |= bit;
                };
                bindSlot("albedo",    0, kTexAlbedo);
                bindSlot("normal",    1, kTexNormal);
                bindSlot("metallic",  2, kTexMetallic);
                bindSlot("roughness", 3, kTexRoughness);
                bindSlot("ao",        4, kTexAO);

                objData.materialPBR = glm::vec4(mat.metallic, mat.roughness, mat.ao, (float)texMask);
            }

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

        // --- Pase INSTANCIADO de piezas de construcción (mismo render pass/depth que la escena) ---
        if ((!constInstances.empty() || !primInstances.empty()) && RHI::valid(m_scenePSO)) {
            HARUKA_PROFILE("scene.construction.instanced");
            if (!RHI::valid(m_constInstPSO)) {                    // PSO instanciado (una vez)
                using V = Haruka::Renderer::Vertex;
                const std::string vs = Shader::baseDir() + "shaders/construction_inst.vert";
                const std::string fs = Shader::baseDir() + "shaders/construction_inst.frag";
                RHI::PipelineDesc pd;
                pd.vertexPath   = vs.c_str();
                pd.fragmentPath = fs.c_str();
                pd.vertexLayout.strides    = { (uint32_t)sizeof(V) };            // binding 0 = Vertex
                pd.vertexLayout.attributes = {
                    { 0, (uint32_t)offsetof(V, Position), RHI::Format::RGB32F, 0 },
                    { 1, (uint32_t)offsetof(V, Normal),   RHI::Format::RGB32F, 0 },
                };
                Haruka::Renderer::GPUInstancing::appendInstanceLayout(pd.vertexLayout, 1);  // binding 1 = instancias
                pd.topology     = RHI::PrimitiveTopology::Triangles;
                pd.depth.test   = true;  pd.depth.write = true;
                pd.blend.enable = false;
                pd.cull         = RHI::CullMode::Back;
                m_constInstPSO  = uboDev->createPipeline(pd);
            }
            if (RHI::valid(m_constInstPSO)) {
                if (!_instancing) { _instancing = std::make_unique<GPUInstancing>(); _instancing->init(20000); }
                sceneCtx->bindPipeline(m_constInstPSO);
                sceneCtx->bindUniformBuffer(0, m_uboPerFrameH);   // view/proj + luces (mismo UBO que la escena)

                // El buffer de instancias es FIJO (`addInstance` descarta al pasarse), y un edificio
                // tejido puede traer miles de piezas → se dibuja POR LOTES del tamaño del buffer, así
                // no desaparece ninguna. `fill` sube el lote [off, off+n) y el llamador dispara el draw.
                const std::size_t cap = (std::size_t)std::max(1, _instancing->getMaxInstances());
                auto fillChunk = [&](const std::vector<Haruka::InstanceDataFloat>& src, std::size_t off) {
                    const std::size_t n = std::min(cap, src.size() - off);
                    _instancing->clear();
                    for (std::size_t i = 0; i < n; ++i)
                        _instancing->addInstance(src[off + i].model, src[off + i].color, src[off + i].scale);
                };

                for (auto& kv : constInstances) {
                    Model* model = AppInternal::getOrLoadModelCached(kv.first);
                    if (!model) continue;
                    // La carga/creación PEREZOSA de arriba crea recursos GL y deja el VAO a 0 → habría
                    // desbindeado el VAO del pipeline justo antes del draw (GL_INVALID_OPERATION).
                    // Re-bindear tras resolver la malla. Misma trampa que documenta el pase de escena.
                    sceneCtx->bindPipeline(m_constInstPSO);
                    for (std::size_t off = 0; off < kv.second.size(); off += cap) {
                        fillChunk(kv.second, off);
                        model->drawInstancedRHI(*sceneCtx, *_instancing, 1);   // 1 draw por malla del modelo
                        ++renderedDrawCalls;
                    }
                }
                // Primitivas (los cubos de los muros tejidos): mismo camino que una malla del modelo —
                // bindear su VBO/EBO y disparar el draw instanciado.
                for (auto& kv : primInstances) {
                    SimpleMesh* pm = AppInternal::getPrimitiveMesh((Haruka::PrimitiveType)kv.first);
                    if (!pm || pm->getIndexCount() == 0) continue;
                    if (!RHI::valid(pm->vertexBuffer()) || !RHI::valid(pm->indexBuffer())) continue;
                    sceneCtx->bindPipeline(m_constInstPSO);   // ver la nota de arriba (creación perezosa)
                    for (std::size_t off = 0; off < kv.second.size(); off += cap) {
                        fillChunk(kv.second, off);
                        sceneCtx->bindVertexBuffer(pm->vertexBuffer(), 0);
                        sceneCtx->bindIndexBuffer(pm->indexBuffer());
                        _instancing->render(sceneCtx, (uint32_t)pm->getIndexCount(), 1);
                        ++renderedDrawCalls;
                        renderedVertices  += pm->getVertexCount()   * (int)std::min(cap, kv.second.size() - off);
                        renderedTriangles += pm->getTriangleCount() * (int)std::min(cap, kv.second.size() - off);
                    }
                }
                sceneCtx->bindPipeline(m_scenePSO);   // restaura el PSO de escena (el cierre del pass lo asume)
            }
        }

        if (RHI::valid(m_scenePSO)) sceneCtx->endRenderPass();

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

                RHI::ClearValues shadowClear;
                shadowClear.clearColor = false; shadowClear.clearDepth = true; shadowClear.depth = 1.0f;
                sceneCtx->beginRenderPass(_shadow->pass(), shadowClear);
                _gameInterface->onRenderShadow(lightSpace, glm::vec3(_camera->position));
                shadowsOn = true;
                sceneCtx->endRenderPass();
            }

            // --- MÁSCARA DE EXPOSICIÓN AL CIELO (pase CENITAL) -----------------------------------
            // El mismo truco que el shadow map, pero con la "luz" en el CÉNIT: lo que aparece aquí
            // es lo que tienes ENCIMA. Es la pieza que el depth buffer no puede dar — la gota que cae
            // entre tu cara y el tejado no tiene nada DELANTE, así que el depth no la descarta; lo que
            // la descarta es saber que hay tejado ARRIBA. Una sola máscara sirve para las tres cosas:
            // nada de lluvia bajo cubierto, la silueta SECA bajo cualquier collider, y dónde cuaja la
            // nieve. Reusa el hook `onRenderShadow` del juego (props, edificios, piezas colocadas):
            // no hay que registrar casters nuevos, ya son los mismos.
            // Se hace SOLO cuando precipita: sin lluvia ni nieve no la lee nadie y sería un pase de
            // profundidad regalado cada frame.
            m_skyMaskOn = false;
            if (_camera && _gameInterface && _gameInterface->onRenderShadow &&
                (m_rainAmount > 0.01f || m_snowAmount > 0.01f || m_groundWetness > 0.01f)) {
                if (!m_skyMask) m_skyMask = std::make_unique<Shadow>(kSkyMaskRes, kSkyMaskRes);
                const glm::dvec3 camD2 = glm::dvec3(_camera->position);
                glm::dvec3 upD(0, 1, 0);
                {                   glm::dvec3 pc2; double pr2;
                    if (_planetarySystem->getActivePlanet(pc2, pr2)) {
                        const glm::dvec3 r = camD2 - pc2; const double rl = glm::length(r);
                        if (rl > 1e-9) upD = r / rl;
                    } }
                const glm::vec3 zen = glm::vec3(upD);
                glm::vec3 zup = (std::abs(zen.y) < 0.95f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
                // Ortográfica MIRANDO HACIA ABAJO desde muy arriba (D), cubriendo ±S alrededor del
                // jugador. D generoso: un tejado o la copa de un árbol a 40 m tiene que entrar, o
                // "sin cubierta" saldría falso justo bajo lo que más tapa.
                const float D = 140.0f, S = (float)kSkyMaskExtentM;
                const glm::mat4 zView = glm::lookAt(zen * D, glm::vec3(0.0f), zup);
                const glm::mat4 zProj = glm::ortho(-S, S, -S, S, 1.0f, 2.0f * D);
                m_skySpace = zProj * zView;

                {
                RHI::ClearValues maskClear;
                maskClear.clearColor = false; maskClear.clearDepth = true; maskClear.depth = 1.0f;
                sceneCtx->beginRenderPass(m_skyMask->pass(), maskClear);
                _gameInterface->onRenderShadow(m_skySpace, glm::vec3(_camera->position));
                m_skyMaskOn = true;
                sceneCtx->endRenderPass();
                }
            }

            // Restore scene framebuffer + viewport before drawing SimplePlanet/game objects
            // (shadow/sky-mask passes changed both and GL's endRenderPass is a no-op).
            {
                RHI::ClearValues restore; restore.clearColor = false; restore.clearDepth = false;
                sceneCtx->beginRenderPass(sceneTargetPass, restore);
                if (!RHI::valid(sceneTargetPass))
                    sceneCtx->setViewport(0, 0, (int)width, (int)height);
            }

            // --- SimplePlanet terrain rendering ---
            {
                HARUKA_PROFILE("simple_planet.draw");
                const float aspectC = (height > 0u) ? (float)width / (float)height : 1.0f;
                const glm::mat4 projC    = _camera->getProjectionMatrix(aspectC);
                const glm::mat4 viewC    = _camera->getViewMatrix();
                for (size_t i = 0; i < _planetarySystem->getSimplePlanetCount(); ++i) {
                    const auto& sp = _planetarySystem->getSimplePlanet(i);
                    _planetarySystem->renderSimplePlanet(sp.name,
                        glm::dvec3(_camera->position), projC, viewC);
                }
            }
        }
    }

    if (_gameInterface && _gameInterface->onRenderWorld && _camera) {
        const float      aspect  = (height > 0u) ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
        const glm::mat4  view    = _camera->getViewMatrix();
        const glm::mat4  proj    = _camera->getProjectionMatrix(aspect);
        const glm::vec3  camPos  = glm::vec3(_camera->position);
        _gameInterface->onRenderWorld(view, proj, camPos);
    }

    // --- PRECIPITACIÓN (lluvia / nieve) — EN EL MUNDO, no sobre la imagen ---------------------
    // Va AQUÍ y no al final del frame a propósito: este es el último punto en el que el target de
    // escena y su DEPTH siguen bindeados. Dibujada después del composite, la gota no tendría contra
    // qué probarse y volveríamos al filtro de pantalla que se acaba de quitar.
    if (m_rainAmount > 0.01f || m_snowAmount > 0.01f) {
        HARUKA_PROFILE("precip.draw");
        const glm::dvec3 camD = glm::dvec3(_camera->position);
        glm::dvec3 upD(0, 1, 0);
        if (_planetarySystem) {
            glm::dvec3 pc; double pr;
            if (_planetarySystem->getActivePlanet(pc, pr)) {
                const glm::dvec3 r = camD - pc; const double rl = glm::length(r);
                if (rl > 1e-9) upD = r / rl;
            }
        }
        Haruka::PrecipitationRenderer::Params pp;
        pp.cameraPos   = camD;
        pp.up          = glm::vec3(upD);
        pp.lookDir     = _camera->getFront();
        pp.wind        = m_windVec;
        pp.sunColor    = _worldSystem ? _worldSystem->getDominantLightColor(camD) : glm::vec3(1.0f);
        pp.snow        = (m_snowAmount > 0.01f);
        pp.amount      = pp.snow ? m_snowAmount : m_rainAmount;
        // TODO (fase C): `skyVisibility` saldrá de la máscara cenital — hoy 1.0, así que bajo un
        // tejado la gota que está ENTRE el tejado y tu cara sigue viéndose (el depth solo descarta
        // lo que tiene algo DELANTE). Es lo único de "no llueve bajo cubierto" que falta.
        pp.skyVisibility = 1.0f;
        pp.timeSeconds = _planetarySystem ? _planetarySystem->simulationTime() : 0.0;
        pp.viewportHeightPx = (int)(m_postActive ? renderH : height);
        if (m_skyMaskOn && m_skyMask) {          // fase C: cada gota mira si tiene algo ENCIMA
            pp.skyMask  = RHI::device()->getDepthTexture(m_skyMask->pass());
            pp.skySpace = m_skySpace;
        }
        m_precip.render(pp);

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

        // RHI migration: GL state transitions removed
    }

    // (Aquí estaba el pase de LLUVIA DE PANTALLA: un triángulo fullscreen con rayas, mezclado sobre
    //  la imagen ya compuesta. Se ha ELIMINADO. Ese pase no tenía profundidad ni posición, así que
    //  llovía dentro de las cuevas, detrás de los muros y bajo los tejados — el "sale donde sea" que
    //  reportó el autor. La precipitación se dibuja ahora COMO GEOMETRÍA dentro del pase de escena
    //  (`PrecipitationRenderer`, justo tras onRenderWorld), sometida al depth buffer del mundo.)

    // Devolver la PANTALLA como framebuffer atado antes de salir. En GL `endRenderPass` es un
    // no-op, así que al terminar la escena sigue atado el FBO del último pase — con `_editorTarget`,
    // el RenderTarget del viewport. Quien nos llama (el IDE) dibuja su ImGui justo después dando por
    // hecho que el framebuffer es la pantalla: sin esto, el UI ENTERO se pinta dentro del target del
    // viewport y el backbuffer no lo escribe nadie, así que la ventana alterna entre dos buffers
    // viejos → parpadea entera.
    // El VIEWPORT lo deja el llamante: en modo editor `_window` es null y `width`/`height` son los
    // del viewport, no los de la ventana — fijarlo aquí sería adivinar. El IDE lo hace con el tamaño
    // real justo antes de su `glClear`.
    if (_editorTarget) {
        if (RHI::Device* dev = RHI::device()) {
            RHI::Context* ctx = dev->beginFrame();
            RHI::ClearValues keep; keep.clearColor = false; keep.clearDepth = false;
            ctx->beginRenderPass(RHI::RenderPassHandle{}, keep);   // id 0 = backbuffer (pantalla)
            ctx->endRenderPass();
        }
    }

    // Clean screenshot (no HUD): capture here, after the 3D scene is composited to
    // the screen but BEFORE ImGui draws over it.
    if (!_editorTarget) captureScreenshotIfPending((int)width, (int)height);

    if (_imguiCallback) {
        _imguiCallback();
    }

    // GPU timer disabled during RHI migration
}

unsigned int Application::getMaterialTextureGL(const std::string& path) {
    RHI::TextureHandle tex = AppInternal::getOrLoadMaterialTexture(path);
    if (!RHI::valid(tex)) return 0;
    RHI::Device* dev = RHI::device();
    return dev ? dev->nativeTexture(tex) : 0;
}

void Application::renderMaterialPreview(const Haruka::MaterialComponent& material,
                                        Haruka::Renderer::RenderTarget& target) {
    RHI::Device* dev = RHI::device();
    if (!dev || !RHI::valid(target.getPass())) return;

    // PSO propio, no el de la escena: `m_scenePSO` alterna entre final.frag y preview.frag según los
    // ajustes de render del usuario, y una preview que cambia de aspecto al tocar un ajuste global
    // no sirve para juzgar un material. Éste se queda SIEMPRE en el look final.
    if (!RHI::valid(m_matPreviewPSO)) {
        const std::string vsPath = Shader::baseDir() + "shaders/simple.vert";
        const std::string fsPath = Shader::baseDir() + "shaders/final.frag";
        using V = Haruka::Renderer::Vertex;
        RHI::PipelineDesc pd;
        pd.vertexPath           = vsPath.c_str();
        pd.fragmentPath         = fsPath.c_str();
        pd.vertexLayout.strides = { (uint32_t)(sizeof(V)) };
        pd.vertexLayout.attributes = {
            { 0, (uint32_t)offsetof(V, Position),  RHI::Format::RGB32F },
            { 1, (uint32_t)offsetof(V, Normal),    RHI::Format::RGB32F },
            { 2, (uint32_t)offsetof(V, TexCoords), RHI::Format::RG32F  },
            { 3, (uint32_t)offsetof(V, Tangent),   RHI::Format::RGB32F },
            { 4, (uint32_t)offsetof(V, Bitangent), RHI::Format::RGB32F },
        };
        pd.topology     = RHI::PrimitiveTopology::Triangles;
        pd.depth.test   = true;  pd.depth.write = true;
        pd.blend.enable = false;
        pd.cull         = RHI::CullMode::Back;
        m_matPreviewPSO = dev->createPipeline(pd);
    }
    if (!RHI::valid(m_matPreviewPSO)) return;
    if (!RHI::valid(m_uboPerFrameH))
        m_uboPerFrameH = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PerFrameUBOData), nullptr, RHI::BufferMemory::Dynamic);
    if (!RHI::valid(m_uboPerObjectH))
        m_uboPerObjectH = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PerObjectUBOData), nullptr, RHI::BufferMemory::Dynamic);

    SimpleMesh* sphere = AppInternal::getPrimitiveMesh(Haruka::PrimitiveType::SPHERE);
    if (!sphere) return;

    // Cámara e iluminación FIJAS: dos materiales solo se pueden comparar si se miran igual. La luz
    // de tres cuartos (arriba-izquierda-delante) es la que enseña a la vez el lado iluminado, el
    // terminador y el rim — que es donde se lee un material.
    // La esfera primitiva es de radio 1 y el FOV es de 35°: a distancia d subtiende asin(1/d), que
    // tiene que caber en el semiángulo de 17.5°. A 2.6 la esfera era MÁS GRANDE que el encuadre y
    // salía recortada en un cuadrado con las esquinas redondeadas. 4.0 → 14.5°, con aire alrededor.
    const float kDist = 4.0f;
    PerFrameUBOData f{};
    f.view       = glm::mat4(glm::mat3(glm::lookAt(glm::vec3(0, 0, kDist), glm::vec3(0), glm::vec3(0, 1, 0))));
    f.projection = glm::perspective(glm::radians(35.0f), 1.0f, 0.05f, 100.0f);
    f.cameraPos  = glm::vec3(0, 0, kDist);
    f.sunDirection    = glm::normalize(glm::vec3(-0.45f, 0.65f, 0.62f));
    f.sunLightColor   = glm::vec3(1.0f, 0.97f, 0.92f);
    f.ambientStrength = 0.25f;
    f.enableHDR = 1; f.enableBloom = 0; f.enableSSAO = 0; f.enableIBL = 0; f.enableShadows = 0;
    f.moonDirection = glm::vec3(0, 1, 0); f.moonIntensity = 0.0f;
    f.moonLightColor = glm::vec3(0.0f);
    dev->updateBuffer(m_uboPerFrameH, 0, sizeof(f), &f);

    PerObjectUBOData o{};
    o.model = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, -kDist));  // la view solo lleva rotación
    o.baseColorAndPlanetRadius = glm::vec4(material.albedo, 1.0f);
    o.planetCenterAndFlag      = glm::vec4(0.0f);
    o.materialEmission         = glm::vec4(material.emission, 0.0f);

    RHI::Context* ctx = dev->beginFrame();
    RHI::ClearValues clear;
    clear.clearColor = true;
    clear.color[0] = 0.09f; clear.color[1] = 0.10f; clear.color[2] = 0.12f; clear.color[3] = 1.0f;
    clear.clearDepth = true; clear.depth = 0.0f;   // reversed-Z
    ctx->beginRenderPass(target.getPass(), clear);
    ctx->bindPipeline(m_matPreviewPSO);

    int texMask = 0;
    auto bindSlot = [&](const char* key, uint32_t unit, int bit) {
        auto it = material.textures.find(key);
        if (it == material.textures.end() || it->second.empty()) return;
        RHI::TextureHandle tex = AppInternal::getOrLoadMaterialTexture(it->second);
        if (!RHI::valid(tex)) return;
        ctx->bindTexture(unit, tex);
        texMask |= bit;
    };
    bindSlot("albedo",    0, kTexAlbedo);
    bindSlot("normal",    1, kTexNormal);
    bindSlot("metallic",  2, kTexMetallic);
    bindSlot("roughness", 3, kTexRoughness);
    bindSlot("ao",        4, kTexAO);
    o.materialPBR = glm::vec4(material.metallic, material.roughness, material.ao, (float)texMask);
    dev->updateBuffer(m_uboPerObjectH, 0, sizeof(o), &o);

    ctx->bindUniformBuffer(0, m_uboPerFrameH);
    ctx->bindUniformBuffer(1, m_uboPerObjectH);
    sphere->drawRHI(*ctx);
    ctx->endRenderPass();

    // Devolver la pantalla: en GL el fin de pase no desata nada y quien llama (el editor) dibuja
    // su ImGui a continuación. Misma razón que en renderFrameContent.
    RHI::ClearValues keep; keep.clearColor = false; keep.clearDepth = false;
    ctx->beginRenderPass(RHI::RenderPassHandle{}, keep);
    ctx->endRenderPass();
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

    if (RHI::device())
        RHI::device()->endFrame();

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
    if (RHI::Device* dev = RHI::device()) {
        dev->readPixels(0, 0, width, height, RHI::Format::RGBA8, buf.data());
    }

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
