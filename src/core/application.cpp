#include "application.h"

#include <iostream>
#include <unordered_map>
#include <algorithm>
#include <csignal>
#include <atomic>
#include <filesystem>
#include <cstring>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>

#include <glm/gtx/euler_angles.hpp>

#include "renderer/motor_instance.h"
#include "game/planetary_system.h"
#include "core/components/mesh_renderer_component.h"
#include "renderer/model.h"
#include "renderer/primitive_shapes.h"
#include "core/scene/scene_render_policy.h"
#include "tools/object_types.h"
#include "tools/error_reporter.h"
#include "tools/profiler.h"
#include "settings/settings_manager.h"

// std140-compatible structs mirroring the UBO declarations in the shaders.
// Any change to the GLSL UBO layout must be reflected here.
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
};
static_assert(sizeof(PerFrameUBOData) == 208, "PerFrameUBOData std140 size mismatch");

struct alignas(16) PerObjectUBOData {
    glm::mat4 model;
    glm::vec4 baseColorAndPlanetRadius; // rgb=color, a=planetRadius
    glm::vec4 planetCenterAndFlag;      // xyz=planetCenter, w=useProceduralTerrain
};
static_assert(sizeof(PerObjectUBOData) == 96, "PerObjectUBOData std140 size mismatch");

static std::atomic<bool> g_sigintReceived{false};
static void handleSigint(int) { g_sigintReceived.store(true); }

namespace {
std::vector<Haruka::RenderCommand> g_sceneRenderQueue;

// GL-owning caches at namespace scope so cleanupGLStatics() can reset them
// before the GL context is destroyed. Function-local statics would destruct
// at program exit (after main() returns), which is after the context is gone.
std::unordered_map<std::string, std::shared_ptr<Model>> g_modelCache;
std::unique_ptr<SimpleMesh> g_sphereMesh;
std::unique_ptr<SimpleMesh> g_cubeMesh;
std::unique_ptr<SimpleMesh> g_capsuleMesh;
std::unique_ptr<SimpleMesh> g_planeMesh;

void cleanupGLStatics() {
    g_modelCache.clear();
    g_sphereMesh.reset();
    g_cubeMesh.reset();
    g_capsuleMesh.reset();
    g_planeMesh.reset();
}

glm::mat4 getTransformMatrix(const Haruka::SceneObject& obj) {
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(obj.position));

    glm::mat4 rotation = glm::eulerAngleXYZ(
        glm::radians(static_cast<float>(obj.rotation.x)),
        glm::radians(static_cast<float>(obj.rotation.y)),
        glm::radians(static_cast<float>(obj.rotation.z))
    );

    transform *= rotation;
    return glm::scale(transform, glm::vec3(obj.scale));
}

Model* getOrLoadModelCached(const std::string& path) {
    auto it = g_modelCache.find(path);
    if (it != g_modelCache.end()) {
        return it->second.get();
    }

    auto model = std::make_shared<Model>(path);
    Model* modelPtr = model.get();
    g_modelCache.emplace(path, std::move(model));
    return modelPtr;
}

SimpleMesh* getPrimitiveMesh(Haruka::PrimitiveType primitive) {
    switch (primitive) {
        case Haruka::PrimitiveType::CUBE:
            if (!g_cubeMesh) {
                std::vector<glm::vec3> vertices;
                std::vector<glm::vec3> normals;
                std::vector<unsigned int> indices;
                PrimitiveShapes::createCube(1.0f, vertices, normals, indices);
                g_cubeMesh = std::make_unique<SimpleMesh>(vertices, normals, indices);
            }
            return g_cubeMesh.get();
        case Haruka::PrimitiveType::SPHERE:
            if (!g_sphereMesh) {
                std::vector<glm::vec3> vertices;
                std::vector<glm::vec3> normals;
                std::vector<unsigned int> indices;
                PrimitiveShapes::createSphereLOD(1.0f, 24, 16, vertices, normals, indices);
                g_sphereMesh = std::make_unique<SimpleMesh>(vertices, normals, indices);
            }
            return g_sphereMesh.get();
        case Haruka::PrimitiveType::CAPSULE:
            if (!g_capsuleMesh) {
                std::vector<glm::vec3> vertices;
                std::vector<glm::vec3> normals;
                std::vector<unsigned int> indices;
                PrimitiveShapes::createCapsule(0.5f, 1.5f, 24, 12, vertices, normals, indices);
                g_capsuleMesh = std::make_unique<SimpleMesh>(vertices, normals, indices);
            }
            return g_capsuleMesh.get();
        case Haruka::PrimitiveType::PLANE:
            if (!g_planeMesh) {
                std::vector<glm::vec3> vertices;
                std::vector<glm::vec3> normals;
                std::vector<unsigned int> indices;
                PrimitiveShapes::createPlane(1.0f, 1.0f, 1, vertices, normals, indices);
                g_planeMesh = std::make_unique<SimpleMesh>(vertices, normals, indices);
            }
            return g_planeMesh.get();
        default:
            return nullptr;
    }
}
}

Application::Application() : _window(nullptr) {
    _frameStart = std::chrono::high_resolution_clock::now();
}

Application::~Application() {
    cleanup();
}

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

// Resolve a map reference to an actual file. Accepts a full path (used as-is) or
// a bare name/path without extension. Dev: prefer the readable .scene; release:
// fall back to the packed .hmap. So init.cpp can ask for "scenes/main" and it
// works in both modes.
static std::string resolveScenePath(const std::string& ref) {
    namespace fs = std::filesystem;
    if (ref.empty()) return ref;
    // Already a concrete file that exists → use it.
    if (fs::exists(ref)) return ref;
    // Has an explicit extension we recognise but doesn't exist → try the sibling.
    auto tryExt = [&](const std::string& base) -> std::string {
        if (fs::exists(base + ".scene")) return base + ".scene"; // dev first
        if (fs::exists(base + ".hmap"))  return base + ".hmap";  // release
        return {};
    };
    // Strip a known extension to get the base name.
    std::string base = ref;
    for (const char* e : { ".scene", ".hmap" }) {
        size_t n = std::strlen(e);
        if (base.size() >= n && base.compare(base.size()-n, n, e) == 0) {
            base = base.substr(0, base.size()-n); break;
        }
    }
    std::string found = tryExt(base);
    return found.empty() ? ref : found;
}

void Application::loadScene(const std::string& scenePathRef) {
    _ownedScene = std::make_unique<Haruka::SceneManager>();
    Haruka::SceneLoader loader(*_ownedScene);

    const std::string scenePath = resolveScenePath(scenePathRef);
    if (!scenePath.empty() && loader.loadFromFile(scenePath)) {
        _currentScene = _ownedScene.get();
        if (_worldSystem) _worldSystem->syncFromScene(*_currentScene);
        initPlanetarySystem();
        std::cout << "[Application] Scene loaded: " << scenePath << std::endl;
        return;
    }

    _currentScene = _ownedScene.get();
    if (_worldSystem) _worldSystem->syncFromScene(*_currentScene);
    initPlanetarySystem();
    std::cout << "[Application] Using empty scene" << std::endl;
}

void Application::setEditorViewportSize(int w, int h) {
    m_editorViewportW = w;
    m_editorViewportH = h;
}

void Application::initPlanetarySystem() {
    if (!_currentScene) return;

    _planetarySystem = std::make_unique<Haruka::PlanetarySystem>();
    _planetarySystem->init();

    // Wire up planetary physics so gravity and terrain collision use real planet data.
    if (_physicsEngine)
        _physicsEngine->initPlanetaryPhysics(
            _worldSystem.get(),
            _planetarySystem.get(),
            _raycastSystem.get());

    for (const auto& objPtr : _currentScene->getAllObjects()) {
        if (!objPtr) continue;
        const auto& obj = *objPtr;

        if (!obj.terrainSettings || obj.terrainSettings->layers.empty()) continue;

        const auto& ts = *obj.terrainSettings;

        Haruka::PlanetarySystem::Planet planet;
        planet.name     = obj.name;
        planet.position = obj.position;
        planet.radius   = std::max({obj.scale.x, obj.scale.y, obj.scale.z});

        // Pass the FULL unfiltered config (every layer param the scene authored),
        // not just the 3 typed fields. Falls back to the typed subset if a scene
        // somehow lacks rawConfig (older saves).
        if (!ts.rawConfig.is_null() && ts.rawConfig.is_object()) {
            planet.terrainSettings["config"] = ts.rawConfig;
            if (!planet.terrainSettings["config"].contains("chunkSize"))
                planet.terrainSettings["config"]["chunkSize"] = ts.chunkSize > 0 ? ts.chunkSize : 32;
        } else {
            planet.terrainSettings["config"]["chunkSize"] = ts.chunkSize > 0 ? ts.chunkSize : 32;
            planet.terrainSettings["config"]["seed"]      = ts.seed;
            for (const auto& [name, layer] : ts.layers) {
                planet.terrainSettings["config"]["layers"][name]["freq"]     = layer.freq;
                planet.terrainSettings["config"]["layers"][name]["octaves"]  = layer.octaves;
                planet.terrainSettings["config"]["layers"][name]["strength"] = layer.strength;
            }
        }

        _planetarySystem->addPlanet(planet);
    }
}

void Application::applyGraphicsSettings() {
    const auto& g = Haruka::SettingsManager::get().graphics();

    setRenderFeatureBloom(g.bloom);
    setRenderFeatureSSAO(g.ssao);
    setRenderFeatureShadows(g.shadowQuality != Haruka::Settings::ShadowQuality::Off);
    setRenderQualityPreset(static_cast<int>(g.shadowQuality)); // 0=Off/Low..3=High

    if (_camera)
        _camera->zoom = g.fov;

    if (_window)
        SDL_GL_SetSwapInterval(g.vsync ? 1 : 0);
}

void Application::init(Haruka::SceneManager& scene) {
    _currentScene = &scene;
    applyGraphicsSettings();

    if (!_worldSystem) {
        _worldSystem = std::make_unique<Haruka::WorldSystem>();
        _worldSystem->init();
    }
    _worldSystem->syncFromScene(scene);

    if (!_physicsEngine) {
        _physicsEngine = std::make_unique<Haruka::PhysicsEngine>();
    }

    if (!_camera) {
        glm::vec3 camStart(0.0f, 0.0f, 5.0f);
        for (const auto& objPtr : scene.getAllObjects()) {
            if (!objPtr || objPtr->type != "Camera") continue;
            camStart = glm::vec3(objPtr->position);
            break;
        }
        _camera = std::make_unique<Camera>(camStart);
    }

    MotorInstance::getInstance().setApplication(this);
    MotorInstance::getInstance().setScene(_currentScene);
    MotorInstance::getInstance().setCamera(_camera.get());
}

void Application::recreateFBOs(int newWidth, int newHeight) {
    _window->setWidth(newWidth);
    _window->setHeight(newHeight);

    uint32_t width = _window->getWidth();
    uint32_t height = _window->getHeight();

    // 1. Actualizar el Viewport global de OpenGL
    glViewport(0, 0, width, height);

    // 2. Recrear el G-Buffer (Esencial para Deferred Rendering)
    // El G-Buffer suele contener texturas de Albedo, Normales, Posición, etc.
    _gBuffer = std::make_unique<GBuffer>(width, height);

    // 3. Recrear buffers de Iluminación y Post-procesado
    _hdr = std::make_unique<HDR>(width, height);
    _bloom = std::make_unique<Bloom>(width, height);

    // 4. Recrear SSAO (requiere el nuevo tamaño para el ruido y samples)
    if (_ssao) {
        _ssao = std::make_unique<SSAO>(width, height);
    }

    // 5. Actualizar la matriz de proyección de la cámara
    if (_camera) {
        _camera->setAspectRatio((float)width / (float)height);
    }
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

    glViewport(0, 0, width, height);
    glClearColor(0.01f, 0.01f, 0.01f, 1.0f);
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
        frameData.ambientStrength = 0.25f;
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

            glm::vec3 baseColor = glm::vec3(obj->color);
            if (glm::length(baseColor) < 0.001f) baseColor = glm::vec3(0.75f, 0.76f, 0.80f);

            const bool isStar = (Haruka::stringToObjectType(obj->type) == Haruka::ObjectType::STAR)
                                 || obj->flags.castLight;

            // Stars emit from their light component color, not the default object color.
            if (isStar && obj->components.contains("light")) {
                const auto& lc = obj->components["light"];
                if (lc.contains("color") && lc["color"].is_array() && lc["color"].size() >= 3)
                    baseColor = glm::vec3(lc["color"][0].get<float>(),
                                         lc["color"][1].get<float>(),
                                         lc["color"][2].get<float>());
            }

            PerObjectUBOData objData{};
            objData.model                    = glm::translate(glm::mat4(1.0f), -cameraOrigin) * getTransformMatrix(*obj);
            objData.baseColorAndPlanetRadius = glm::vec4(baseColor, 1.0f);
            // w = 0: normal, 1: procedural terrain, 2: emissive star (no diffuse shading)
            objData.planetCenterAndFlag      = glm::vec4(0.0f, 0.0f, 0.0f, isStar ? 2.0f : 0.0f);

            glBindBuffer(GL_UNIFORM_BUFFER, m_uboPerObject);
            glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(PerObjectUBOData), &objData);
            glBindBuffer(GL_UNIFORM_BUFFER, 0);

            switch (command.kind) {
                case Haruka::RenderKind::Model: {
                    Model* model = getOrLoadModelCached(obj->modelPath);
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
                    SimpleMesh* primitiveMesh = getPrimitiveMesh(command.primitive);
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
            _iPendingChunkEvictions = 0;
            _iTrackedChunks         = _iVisibleChunks + _iResidentChunks;
            _iResidentMemoryMB      = _planetarySystem->getCacheMemoryMB();
            _iMaxMemoryMB           = _planetarySystem->getCacheMaxMemoryMB();

            if (_planetShader) {
                _planetShader->use();
                glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_uboPerFrame);
                glBindBufferBase(GL_UNIFORM_BUFFER, 1, m_uboPerObject);
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

            // Bind default FBO in case a previous pass left a custom one bound
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

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
                terrainObj.planetCenterAndFlag      = glm::vec4(0.0f);

                glBindBuffer(GL_UNIFORM_BUFFER, m_uboPerObject);
                glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(PerObjectUBOData), &terrainObj);
                glBindBuffer(GL_UNIFORM_BUFFER, 0);

                _planetarySystem->renderPlanetTerrain(planet.name, glm::dvec3(_camera->position));
            }
            }

            // --- Water pass (ocean shell, after terrain) ---
            if (_waterShader) {
                HARUKA_PROFILE("water.draw");
                _waterShader->use();
                glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_uboPerFrame);
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LEQUAL);
                glDepthMask(GL_TRUE);
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
                    // Wind: constant for now. Future storm system drives these per region.
                    glUniform2f(18, 1.0f, 0.0f); // u_windDir (tangent plane)
                    glUniform1f(19, 1.0f);       // u_windStrength
                    glUniform1i(20, 1);          // u_waterQuality (0=spec,1=sky refl) — settings hook

                    _planetarySystem->renderPlanetWater(planet.name, camD);
                }
                glDisable(GL_BLEND);
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

#ifdef HARUKA_NETWORK
void Application::sendPlayerTransform(uint32_t uuid, const Haruka::WorldPos& pos, const Haruka::Rotation& rot) {
    if (!m_dgs.isConnected()) return;
    int32_t cx = (int32_t)std::floor(pos.x / Haruka::Units::KM);
    int32_t cy = (int32_t)std::floor(pos.y / Haruka::Units::KM);
    int32_t cz = (int32_t)std::floor(pos.z / Haruka::Units::KM);
    float localPos[3] = {
        (float)(pos.x - cx * Haruka::Units::KM),
        (float)(pos.y - cy * Haruka::Units::KM),
        (float)(pos.z - cz * Haruka::Units::KM)
    };
    float rotF[4] = { (float)rot.x, (float)rot.y, (float)rot.z, (float)rot.w };
    m_dgs.sendTransform(uuid, cx, cy, cz, localPos, rotF);
}

void Application::sendPlayerChat(uint32_t uuid, const std::string& username, const std::string& text) {
    if (!m_dgs.isConnected()) return;
    m_dgs.sendChat(uuid, username, text);
}

std::vector<DGS::ChatMessage> Application::pollPlayerChats() {
    return m_dgs.pollChats();
}

bool Application::connectDGS(const std::string& headHost, int headPort,
                               const std::string& email,    const std::string& password,
                               const std::string& apiHost,  int apiPort) {
    return m_dgs.connect(headHost, headPort, email, password,
                         apiHost.empty() ? headHost : apiHost,
                         apiPort <= 0   ? headPort + 1 : apiPort);
}

bool Application::isNetworkConnected() const { return m_dgs.isConnected(); }
int  Application::getGhostCount()      const { return (int)m_ghostObjects.size(); }
#endif

void Application::cleanup() {
    if (m_cleanedUp) return;
    m_cleanedUp = true;

    g_sceneRenderQueue.clear();

#ifdef HARUKA_NETWORK
    m_dgs.disconnect();
#endif

    MotorInstance::getInstance().clear();

    _currentScene = nullptr;
    _ownedScene.reset();

    _physicsEngine.reset();
    _terrainStreamingSystem.reset();
    _chunkCache.reset();
    _planetarySystem.reset();
    _worldSystem.reset();
    _raycastSystem.reset();

    if (m_uboPerFrame  != 0) { glDeleteBuffers(1, &m_uboPerFrame);  m_uboPerFrame  = 0; }
    if (m_uboPerObject != 0) { glDeleteBuffers(1, &m_uboPerObject); m_uboPerObject = 0; }

    if (quadVBO != 0) {
        glDeleteBuffers(1, &quadVBO);
        quadVBO = 0;
    }
    if (quadVAO != 0) {
        glDeleteVertexArrays(1, &quadVAO);
        quadVAO = 0;
    }

    // Release ALL GL-owned resources before the context is destroyed.
    // Members not explicitly reset here would run their destructors AFTER
    // _window->shutdown() destroys the GL context, corrupting the heap.
    _mainShader.reset();
    _planetShader.reset();
    _lampShader.reset();
    _geomShader.reset();
    _ssaoShader.reset();
    _lightShader.reset();
    _compositeShader.reset();
    _flatShader.reset();
    _cascadeShadowShader.reset();
    _bloomExtractShader.reset();
    _bloomBlurShader.reset();
    _pointShadowShader.reset();
    _instancingShader.reset();

    // Render-pipeline objects with GL resources in their destructors
    _shadow.reset();
    _hdr.reset();
    _bloom.reset();
    _gBuffer.reset();
    _ssao.reset();
    _ibl.reset();
    _pointShadow.reset();
    _lightCuller.reset();
    _instancing.reset();
    _computePostProcess.reset();
    _cascadedShadow.reset();
    _virtualTexturing.reset();

    // Render targets (own FBOs / textures)
    _lightingTarget.reset();
    _bloomExtractTarget.reset();
    _bloomPing.reset();
    _bloomPong.reset();

    // Primitive meshes (own VAOs / VBOs)
    for (auto& lod : sphereLOD) lod.reset();

    // Free GL-owning caches (model cache + primitive meshes created on demand)
    cleanupGLStatics();

    if (_window) {
        _window->shutdown();
    }

    if (SDL_WasInit(SDL_INIT_VIDEO)) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
}

void Application::run(const std::string& startScenePath) {
    uint32_t _width = 1280;
    uint32_t _height = 720;

    _window = std::make_unique<Haruka::Core::Window>(
        Haruka::Core::WindowProps("Haruka Engine", _width, _height)
    );
    if (!_window->init()) {
        HARUKA_MOTOR_ERROR(ErrorCode::WINDOW_CREATION_FAILED, "Failed to initialize Window system.");
        return;
    }

    // GL debug output — catches driver errors and shader compile failures.
    // Synchronous mode ensures the callback fires at the exact offending call.
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(
        [](GLenum /*src*/, GLenum type, GLuint /*id*/, GLenum severity,
           GLsizei /*len*/, const GLchar* msg, const void*) {
            if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
            const char* lvl = (type == GL_DEBUG_TYPE_ERROR) ? "ERROR"
                            : (severity == GL_DEBUG_SEVERITY_HIGH) ? "HIGH"
                            : (severity == GL_DEBUG_SEVERITY_MEDIUM) ? "MEDIUM" : "LOW";
            fprintf(stderr, "[GL %s] %s\n", lvl, msg);
        }, nullptr);
    // Suppress performance notifications — only keep errors/warnings
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE,
                          GL_DEBUG_SEVERITY_NOTIFICATION, 0, nullptr, GL_FALSE);

    // ImGui — standalone runtime owns the context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForOpenGL(_window->getNativeWindow(), _window->getContext());
    ImGui_ImplOpenGL3_Init("#version 450");

    loadScene(startScenePath);
    init(*_currentScene);

    if (_gameInterface && _gameInterface->onInit) {
        _gameInterface->onInit(_currentScene);
    }

    // Re-apply graphics settings after game code has loaded its .ini
    applyGraphicsSettings();

    std::signal(SIGINT,  handleSigint);
    std::signal(SIGTERM, handleSigint);

    bool running = true;
    while (running) {
        if (g_sigintReceived.load()) { running = false; continue; }

        auto now = std::chrono::high_resolution_clock::now();
        deltaTime        = std::chrono::duration<float>(now - _frameStart).count();
        deltaTime        = std::min(deltaTime, 0.1f); // cap: network stalls can't explode physics
        _lastFrameTimeMs = deltaTime * 1000.0f;
        _frameStart      = now;

        uint32_t lastWidth  = _window->getWidth();
        uint32_t lastHeight = _window->getHeight();

        // Poll events — game gets first crack, then ImGui
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            bool consumed = false;
            if (_gameInterface && _gameInterface->onEvent)
                consumed = _gameInterface->onEvent(&event);
            if (!consumed)
                ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_WINDOW_RESIZED) {
                m_editorViewportW = event.window.data1;
                m_editorViewportH = event.window.data2;
                glViewport(0, 0, event.window.data1, event.window.data2);
            }
        }

        if (_window->getWidth() != lastWidth || _window->getHeight() != lastHeight)
            recreateFBOs(_window->getWidth(), _window->getHeight());

        // Snapshot last frame's profiler sections at the TRUE frame boundary so
        // the game update (physics: PBF/XPBD/shallow-water) is measured too.
        Haruka::Profiler::get().newFrame();

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        if (_gameInterface && _gameInterface->onUpdate) {
            HARUKA_PROFILE("game.onUpdate");
            _gameInterface->onUpdate(_window->getNativeWindow(), deltaTime);
        }

        // Sync game camera → engine camera so renderFrameContent uses up-to-date matrices
        if (_gameInterface && _gameInterface->getCamera) {
            Camera* gameCam = _gameInterface->getCamera();
            if (gameCam && _camera)
                *_camera = *gameCam;
        }

        // 3D render pass (calls onRenderWorld inside renderFrameContent)
        buildRenderQueue();
        renderFrameContent();

        // ImGui composite — draw all ImGui widgets over the 3D scene
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glFlush();

        _window->swapBuffers();

        // FPS tracking
        _fpsFrameCount++;
        _fpsLastTime += deltaTime;
        if (_fpsLastTime >= 1.0) {
            _lastFps      = static_cast<float>(_fpsFrameCount / _fpsLastTime);
            _fpsFrameCount = 0;
            _fpsLastTime   = 0.0;
        }
    }

    if (_gameInterface && _gameInterface->onShutdown)
        _gameInterface->onShutdown();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}