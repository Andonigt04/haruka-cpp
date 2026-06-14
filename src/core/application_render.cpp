// Application — render pipeline.
// The per-frame work: build the render queue, the deferred/forward object pass,
// terrain/islands/water passes, the standalone post-processing composite and the
// GPU timer. Lifecycle/orchestration lives in application.cpp; the GL asset
// caches in application_assets.cpp.

#include "application.h"
#include "application_internal.h"

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
    glGenBuffers(1, &quadVBO);
    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
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

unsigned int Application::renderBloom(unsigned int srcColorTex) {
    // Bloom runs at HALF resolution: ~4x fewer pixels through the blur ping-pong
    // for a near-identical look (bloom is low-frequency). The composite samples it
    // at full-res UV and linear-upscales.
    const int bw = std::max(1, m_postW / 2), bh = std::max(1, m_postH / 2);
    // (Re)create the two ping-pong color targets when the size changes.
    if (m_bloomFBO[0] == 0 || m_bloomW != bw || m_bloomH != bh) {
        if (m_bloomFBO[0]) { glDeleteFramebuffers(2, m_bloomFBO); glDeleteTextures(2, m_bloomTex); }
        glGenFramebuffers(2, m_bloomFBO);
        glGenTextures(2, m_bloomTex);
        for (int i = 0; i < 2; ++i) {
            glBindFramebuffer(GL_FRAMEBUFFER, m_bloomFBO[i]);
            glBindTexture(GL_TEXTURE_2D, m_bloomTex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, bw, bh, 0, GL_RGBA, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_bloomTex[i], 0);
        }
        m_bloomW = bw; m_bloomH = bh;
    }
    if (quadVAO == 0) setupQuad();
    if (!_bloomExtractShader)
        _bloomExtractShader = std::make_unique<Shader>("shaders/screenquad.vert", "shaders/bloom_extract.frag");
    if (!_bloomBlurShader)
        _bloomBlurShader = std::make_unique<Shader>("shaders/screenquad.vert", "shaders/bloom_blur.frag");

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glViewport(0, 0, bw, bh);
    glBindVertexArray(quadVAO);

    // 1. Bright-pass: scene color -> tex[0].
    glBindFramebuffer(GL_FRAMEBUFFER, m_bloomFBO[0]);
    _bloomExtractShader->use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, srcColorTex);
    glUniform1f(0, Haruka::SettingsManager::get().graphics().bloomThreshold); // luminance threshold
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // 2. Separable Gaussian: N iterations of horizontal+vertical, ping-ponging
    //    tex[1] <-> tex[0]. Result ends up in tex[0].
    _bloomBlurShader->use();
    unsigned int src = m_bloomTex[0];
    const int iterations = 5;
    for (int i = 0; i < iterations; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_bloomFBO[1]); // horizontal -> tex[1]
        glUniform1i(0, 1);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glBindFramebuffer(GL_FRAMEBUFFER, m_bloomFBO[0]); // vertical -> tex[0]
        glUniform1i(0, 0);
        glBindTexture(GL_TEXTURE_2D, m_bloomTex[1]);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        src = m_bloomTex[0];
    }

    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
    return m_bloomTex[0];
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
    // Cielo atmosférico: color por elevación solar + altitud (azul de día → cálido al
    // amanecer/atardecer → oscuro de noche → negro en el espacio). Fallback oscuro.
    glm::vec3 sky(0.01f);
    if (_worldSystem && _camera) sky = _worldSystem->getSkyColor(glm::dvec3(_camera->position));
    glClearColor(sky.r, sky.g, sky.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    _iTotalDrawCalls    = static_cast<int>(g_sceneRenderQueue.size());
    _iRenderedDrawCalls = 0;
    _iTotalVertices     = 0;
    _iTotalTriangles    = 0;
    _iRenderedVertices  = 0;
    _iRenderedTriangles = 0;

    if (_currentScene && _camera) {
        // Lazy-create UBOs
        if (m_uboPerFrame == 0) {
            glGenBuffers(1, &m_uboPerFrame);
            glBindBuffer(GL_UNIFORM_BUFFER, m_uboPerFrame);
            glBufferData(GL_UNIFORM_BUFFER, sizeof(PerFrameUBOData), nullptr, GL_DYNAMIC_DRAW);
            glBindBuffer(GL_UNIFORM_BUFFER, 0);
        }
        if (m_uboPerObject == 0) {
            glGenBuffers(1, &m_uboPerObject);
            glBindBuffer(GL_UNIFORM_BUFFER, m_uboPerObject);
            glBufferData(GL_UNIFORM_BUFFER, sizeof(PerObjectUBOData), nullptr, GL_DYNAMIC_DRAW);
            glBindBuffer(GL_UNIFORM_BUFFER, 0);
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
        if (!_planetShader) {
            _planetShader = std::make_unique<Shader>(
                "shaders/planet.vert",
                "shaders/planet.frag"
            );
        }
        if (!_waterShader) {
            _waterShader = std::make_unique<Shader>(
                "shaders/water.vert",
                "shaders/water.frag"
            );
        }

        _mainShader->use();
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_uboPerFrame);
        glBindBufferBase(GL_UNIFORM_BUFFER, 1, m_uboPerObject);

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
            frameData.ambientStrength = glm::mix(0.04f, 0.28f, day);
        }
        // Viento atmosférico → arrastre aerodinámico de la física (por cuerpo, barato).
        if (_physicsEngine && _worldSystem)
            _physicsEngine->setWind(_worldSystem->getWind(glm::dvec3(cameraOrigin)));
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

        glBindBuffer(GL_UNIFORM_BUFFER, m_uboPerFrame);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(PerFrameUBOData), &frameData);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);

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

            const bool isStar = (Haruka::stringToObjectType(obj->type) == Haruka::ObjectType::STAR)
                                 || obj->flags.castLight;

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

            glBindBuffer(GL_UNIFORM_BUFFER, m_uboPerObject);
            glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(PerObjectUBOData), &objData);
            glBindBuffer(GL_UNIFORM_BUFFER, 0);

            switch (command.kind) {
                case Haruka::RenderKind::Model: {
                    Model* model = AppInternal::getOrLoadModelCached(obj->modelPath);
                    if (!model) break;
                    model->Draw(*_mainShader);
                    ++renderedDrawCalls;
                    renderedVertices  += model->getVertexCount();
                    renderedTriangles += model->getTriangleCount();
                    break;
                }
                case Haruka::RenderKind::MeshComponent: {
                    if (!obj->meshRenderer || !obj->meshRenderer->isResident()) break;
                    obj->meshRenderer->render(*_mainShader);
                    ++renderedDrawCalls;
                    renderedVertices  += obj->meshRenderer->getResidentVertexCount();
                    renderedTriangles += obj->meshRenderer->getResidentTriangleCount();
                    break;
                }
                case Haruka::RenderKind::Primitive: {
                    SimpleMesh* primitiveMesh = AppInternal::getPrimitiveMesh(command.primitive);
                    if (!primitiveMesh) break;
                    primitiveMesh->draw();
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
                glClear(GL_DEPTH_BUFFER_BIT);
                glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE); glDisable(GL_CULL_FACE);
                _gameInterface->onRenderShadow(lightSpace, glm::vec3(_camera->position));
                shadowsOn = true;
                glBindFramebuffer(GL_FRAMEBUFFER, m_sceneTargetFBO);
                glViewport(0, 0, m_postActive ? (uint32_t)m_postW : width,
                                 m_postActive ? (uint32_t)m_postH : height);
            }

            if (_planetShader) {
                _planetShader->use();
                glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_uboPerFrame);
                glBindBufferBase(GL_UNIFORM_BUFFER, 1, m_uboPerObject);
                // Fog on/off from the config (console: fog 0|1). Persists for the
                // islands pass (reuses the same shader).
                glUniform1i(15, Haruka::SettingsManager::get().graphics().fog ? 1 : 0);
                // Shadows: bind the depth map + light matrix (the terrain receives them).
                if (shadowsOn && _shadow) {
                    glActiveTexture(GL_TEXTURE5);
                    glBindTexture(GL_TEXTURE_2D, _shadow->depthMap);
                    glUniform1i(34, 5);
                    glUniformMatrix4fv(30, 1, GL_FALSE, &lightSpace[0][0]);
                    glUniform1i(35, 1);
                    glActiveTexture(GL_TEXTURE0);
                } else {
                    glUniform1i(35, 0);
                }
            }

            // Ensure correct GL state for terrain pass
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);  // near/far=0.1/3e11 collapses NDC_z to ~1.0 for km-range terrain
            glDepthMask(GL_TRUE);
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glFrontFace(GL_CCW);
            glDisable(GL_BLEND);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

            // Bind the scene target (the offscreen post target when active, else
            // the default FBO) in case a previous pass left a custom one bound.
            glBindFramebuffer(GL_FRAMEBUFFER, m_sceneTargetFBO);
            if (m_postActive)
                glViewport(0, 0, (uint32_t)m_postW, (uint32_t)m_postH);

            {
            HARUKA_PROFILE("terrain.draw");
            // Camera-relative view-projection for frustum culling (matches the
            // shader's projection * mat3(view) — view rotation only, no translation).
            const float aspectC = (height > 0u) ? (float)width / (float)height : 1.0f;
            glm::mat4 camRelVP = _camera->getProjectionMatrix(aspectC)
                               * glm::mat4(glm::mat3(_camera->getViewMatrix()));
            _planetarySystem->setTerrainCullMatrix(camRelVP);
            for (const auto& planet : _planetarySystem->getPlanets()) {
                PerObjectUBOData terrainObj{};
                terrainObj.model                    = glm::mat4(1.0f);
                terrainObj.baseColorAndPlanetRadius = glm::vec4(0.76f, 0.78f, 0.82f, (float)planet.radius);
                // xyz = (camera − planet center) in double→float (precise). The
                // shader reconstructs relPos = FragPos + this → height/slope/climate.
                terrainObj.planetCenterAndFlag      = glm::vec4(
                    glm::vec3(glm::dvec3(_camera->position) - planet.position), 0.0f);

                glBindBuffer(GL_UNIFORM_BUFFER, m_uboPerObject);
                glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(PerObjectUBOData), &terrainObj);
                glBindBuffer(GL_UNIFORM_BUFFER, 0);

                _planetarySystem->renderPlanetTerrain(planet.name, glm::dvec3(_camera->position));
            }
            }


            // --- Floating islands pass (reuse planet shader, opaque, after terrain) ---
            if (_planetShader) {
                HARUKA_PROFILE("islands.draw");
                _planetShader->use();
                glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_uboPerFrame);
                glUniform1f(12, 0.0f); // u_morphFactor = 0 (islands don't morph)
                glUniform1i(11, 0);    // u_terrainMode = procedural
                glDisable(GL_CULL_FACE); // two-sided: the blob's winding can vary
                for (const auto& planet : _planetarySystem->getPlanets())
                    _planetarySystem->renderPlanetIslands(planet.name, glm::dvec3(_camera->position));
                glEnable(GL_CULL_FACE);
            }

            // (Resources —trees/rocks/minerals— are drawn by the GAME in onRenderWorld;
            //  they are game content, not engine content.)

            // --- Water pass (ocean shell, after terrain) ---
            if (_waterShader) {
                HARUKA_PROFILE("water.draw");
                _waterShader->use();
                glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_uboPerFrame);
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LEQUAL);
                // Transparent water must NOT write depth: it depth-TESTS (terrain in front
                // still occludes it) but writing depth would clip whatever is drawn AFTER it
                // (the game's props/resources in onRenderWorld) at the waterline → "water cuts
                // objects". Restored to GL_TRUE right after the pass.
                glDepthMask(GL_FALSE);
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);
                glFrontFace(GL_CCW);
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

                glUniform1f(14, (float)(SDL_GetTicks() / 1000.0)); // u_time

                const glm::dvec3 camD = glm::dvec3(_camera->position);
                for (const auto& planet : _planetarySystem->getPlanets()) {
                    // Camera-anchored tangent basis for the Gerstner waves. Global
                    // per planet/frame → seam-free across all water chunks.
                    glm::dvec3 up = camD - planet.position;
                    double upLen = glm::length(up);
                    up = (upLen > 1e-9) ? up / upLen : glm::dvec3(0.0, 1.0, 0.0);
                    glm::dvec3 ref = (std::abs(up.y) < 0.99) ? glm::dvec3(0,1,0) : glm::dvec3(1,0,0);
                    glm::dvec3 T = glm::normalize(glm::cross(ref, up));
                    glm::dvec3 B = glm::cross(up, T);

                    glm::vec3 upf(up), Tf(T), Bf(B);
                    glUniform3fv(15, 1, &upf[0]);
                    glUniform3fv(16, 1, &Tf[0]);
                    glUniform3fv(17, 1, &Bf[0]);
                    // Oleaje = MAREA (alineación Sol–Luna) × VIENTO atmosférico (ráfagas):
                    // marea viva + ráfaga = mar picado; marea muerta sin viento = calmo.
                    glUniform2f(18, 1.0f, 0.0f); // u_windDir (tangent plane)
                    const float tide = _worldSystem ? _worldSystem->getTideFactor() : 1.0f;
                    float windF = 1.0f;
                    if (_worldSystem) {
                        float ws = glm::length(_worldSystem->getWind(camD));
                        windF = glm::clamp(0.55f + ws * 0.10f, 0.5f, 1.6f);
                    }
                    glUniform1f(19, tide * windF); // u_windStrength (marea × viento)
                    // Water quality drops with altitude: from orbit/space the cheap path
                    // (no sky reflection) — you can't see the detail and it must stay cheap.
                    const double waterAlt = (planet.radius > 1e-9)
                        ? (upLen - planet.radius) / planet.radius : 0.0;
                    glUniform1i(20, waterAlt < 0.05 ? 1 : 0); // 0=spec only, 1=sky refl (near)

                    _planetarySystem->renderPlanetWater(planet.name, camD);
                }
                glDisable(GL_BLEND);
                glDepthMask(GL_TRUE);   // restore for subsequent passes (resources, etc.)
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
    if (m_postActive && _postScene) {
        if (quadVAO == 0) setupQuad();
        if (!_compositeShader)
            _compositeShader = std::make_unique<Shader>("shaders/screenquad.vert",
                                                        "shaders/post_present.frag");

        // Build the blurred bloom texture first (it rebinds FBOs/viewport).
        unsigned int bloomTex = 0;
        if (wantBloom) bloomTex = renderBloom(_postScene->getColorTexture());

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width, height);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);

        _compositeShader->use();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, _postScene->getColorTexture());
        if (wantBloom) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, bloomTex);
        }
        glUniform1i(0, wantFXAA ? 1 : 0);                                 // u_fxaa
        glUniform2f(1, 1.0f / (float)m_postW, 1.0f / (float)m_postH);     // u_texel
        glUniform1i(2, wantBloom ? 1 : 0);                               // u_bloom
        glUniform1f(3, gpost.bloomStrength);                            // u_bloomStrength

        glBindVertexArray(quadVAO);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
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
