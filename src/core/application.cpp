#include "core/game_interface.h"
#include "application.h"

#include <iostream>
#include <cmath>
#include <chrono>
#include <unordered_map>
#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>

#include "renderer/mesh.h"
#include "renderer/motor_instance.h"
#include "core/world_system.h"
#include "renderer/shader.h"
#include "renderer/model.h"
#include "renderer/shadow.h"
#include "renderer/hdr.h"
#include "renderer/bloom.h"
#include "renderer/gbuffer.h"
#include "renderer/ssao.h"
#include "renderer/ibl.h"
#include "renderer/point_shadow.h"
#include "renderer/render_target.h"
#include "renderer/simple_mesh.h"
#include "renderer/primitive_shapes.h"
#include "core/project.h"
#include "tools/error_reporter.h"
#include "tools/startup_report.h"
#include "renderer/gpu_instancing.h"
#include "physics/raycast_simple.h"
#include "physics/physics_engine.h"
#include "tools/object_types.h"
#include "core/terrain/terrain_streaming_system.h"
#include "core/scene/scene_loader.h"
#include "core/chunk_cache.h"

Haruka::GameInterface* gameInterface = nullptr;

namespace {
std::vector<const Haruka::SceneObject*> g_sceneRenderQueue;
Haruka::SceneManager* g_sceneForRender = nullptr;
std::unordered_map<std::string, std::shared_ptr<Model>> g_modelCache;

bool isRenderDisabledByEditor(const Haruka::SceneObject& obj) {
    if (!obj.properties.is_object()) return false;
    if (!obj.properties.contains("terrainEditor")) return false;
    const auto& te = obj.properties["terrainEditor"];
    return te.value("disableRender", false);
}

bool shouldUseProceduralTerrainLook(const Haruka::SceneObject& obj) {
    if (obj.name.find("_chunk_") != std::string::npos) return true;
    if (!obj.properties.is_object()) return false;
    if (!obj.properties.contains("terrainEditor")) return false;
    return obj.properties["terrainEditor"].is_object();
}

bool isTerrainChunkFacingCamera(const Haruka::SceneObject& obj, const glm::vec3& camDirFromPlanet) {
    if (!obj.properties.is_object()) return true;
    if (!obj.properties.contains("terrainEditor")) return true;
    const auto& te = obj.properties["terrainEditor"];
    if (!te.is_object() || !te.value("isChunk", false)) return true;

    if (!te.contains("chunkFace") || !te.contains("chunkX") || !te.contains("chunkY") ||
        !te.contains("chunkTilesX") || !te.contains("chunkTilesY")) return true;

    const int face  = te.value("chunkFace", 0);
    const int tileX = te.value("chunkX", 0);
    const int tileY = te.value("chunkY", 0);
    const int tilesX = te.value("chunkTilesX", 1);
    const int tilesY = te.value("chunkTilesY", 1);
    if (tilesX <= 0 || tilesY <= 0) return true;

    // Cube-sphere direction for this tile's center (same mapping as generateChunkInternal).
    const float u = (static_cast<float>(tileX) + 0.5f) / static_cast<float>(tilesX) * 2.0f - 1.0f;
    const float v = (static_cast<float>(tileY) + 0.5f) / static_cast<float>(tilesY) * 2.0f - 1.0f;
    glm::vec3 cube;
    switch (face) {
        case 0: cube = glm::vec3( 1.0f,  v, -u); break;
        case 1: cube = glm::vec3(-1.0f,  v,  u); break;
        case 2: cube = glm::vec3( u,  1.0f, -v); break;
        case 3: cube = glm::vec3( u, -1.0f,  v); break;
        case 4: cube = glm::vec3( u,  v,  1.0f); break;
        default:cube = glm::vec3(-u,  v, -1.0f); break;
    }
    const glm::vec3 chunkDir = glm::normalize(cube);

    return glm::dot(chunkDir, camDirFromPlanet) > -0.15f;
}

void buildPrimitiveMeshFromProperties(Haruka::SceneObject& obj) {
    if (!obj.meshRenderer)
        obj.meshRenderer = std::make_shared<MeshRendererComponent>();
    if (obj.meshRenderer->isResident()) return;
    if (!obj.properties.contains("meshRenderer")) return;

    const auto& mr = obj.properties["meshRenderer"];
    Haruka::PrimitiveMeshType pmt = Haruka::stringToPrimitiveMeshType(mr.value("meshType", ""));
    if (!Haruka::isPrimitive(pmt)) return;

    std::vector<glm::vec3> verts, norms;
    std::vector<unsigned int> indices;

    switch (pmt) {
        case Haruka::PrimitiveMeshType::CUBE:
            PrimitiveShapes::createCube(mr.value("size", 1.0f), verts, norms, indices);
            break;
        case Haruka::PrimitiveMeshType::SPHERE:
            PrimitiveShapes::createSphere(
                mr.value("radius", 1.0f),
                mr.value("segments", 32), mr.value("segments", 32),
                verts, norms, indices);
            break;
        case Haruka::PrimitiveMeshType::CAPSULE:
            PrimitiveShapes::createCapsule(
                mr.value("radius", 0.5f), mr.value("height", 2.0f),
                mr.value("segments", 24), mr.value("stacks", 16),
                verts, norms, indices);
            break;
        case Haruka::PrimitiveMeshType::PLANE:
            PrimitiveShapes::createPlane(
                mr.value("width", 2.0f), mr.value("height", 2.0f),
                mr.value("subdivisions", 10),
                verts, norms, indices);
            break;
        default: break;
    }
    
    if (!verts.empty()) {

    if (!verts.empty())
        obj.meshRenderer->setMesh(verts, norms, indices);
}

void maybeReleasePrimitiveMesh(Haruka::SceneObject& obj) {
    if (obj.meshRenderer && obj.meshRenderer->isResident())
        obj.meshRenderer->releaseMesh();
}

std::shared_ptr<Model> getOrLoadModel(const std::string& path) {
    auto it = g_modelCache.find(path);
    if (it != g_modelCache.end()) {
        return it->second;
    }

    try {
        auto model = std::make_shared<Model>(path);
        g_modelCache[path] = model;
        return model;
    } catch (...) {
        return nullptr;
    }
}

void releaseModelFromCache(const std::string& path) {
    g_modelCache.erase(path);
}

// Decides whether an object is a primitive or a model and loads it accordingly.
// Called once per object at scene load time and on demand by buildRenderQueue.
void initSceneObjectMesh(Haruka::SceneObject& obj) {
    Haruka::PrimitiveMeshType pmt = Haruka::PrimitiveMeshType::NONE;
    if (obj.properties.contains("meshRenderer"))
        pmt = Haruka::stringToPrimitiveMeshType(
            obj.properties["meshRenderer"].value("meshType", ""));

    if (Haruka::isPrimitive(pmt)) {
        buildPrimitiveMeshFromProperties(obj);
    } else if (!obj.modelPath.empty()) {
        getOrLoadModel(obj.modelPath); // warms the cache; render path picks it up
    }
}

bool isSphereInsideCameraFrustum(
    const glm::dvec3& center,
    double radius,
    const glm::mat4& viewProj
) {
    // Extracción de planos del frustum en espacio mundo desde VP
    glm::vec4 planes[6];

    // left, right, bottom, top, near, far
    planes[0] = glm::vec4(
        viewProj[0][3] + viewProj[0][0],
        viewProj[1][3] + viewProj[1][0],
        viewProj[2][3] + viewProj[2][0],
        viewProj[3][3] + viewProj[3][0]);
    planes[1] = glm::vec4(
        viewProj[0][3] - viewProj[0][0],
        viewProj[1][3] - viewProj[1][0],
        viewProj[2][3] - viewProj[2][0],
        viewProj[3][3] - viewProj[3][0]);
    planes[2] = glm::vec4(
        viewProj[0][3] + viewProj[0][1],
        viewProj[1][3] + viewProj[1][1],
        viewProj[2][3] + viewProj[2][1],
        viewProj[3][3] + viewProj[3][1]);
    planes[3] = glm::vec4(
        viewProj[0][3] - viewProj[0][1],
        viewProj[1][3] - viewProj[1][1],
        viewProj[2][3] - viewProj[2][1],
        viewProj[3][3] - viewProj[3][1]);
    planes[4] = glm::vec4(
        viewProj[0][3] + viewProj[0][2],
        viewProj[1][3] + viewProj[1][2],
        viewProj[2][3] + viewProj[2][2],
        viewProj[3][3] + viewProj[3][2]);
    planes[5] = glm::vec4(
        viewProj[0][3] - viewProj[0][2],
        viewProj[1][3] - viewProj[1][2],
        viewProj[2][3] - viewProj[2][2],
        viewProj[3][3] - viewProj[3][2]);

    glm::vec3 c = glm::vec3(center);
    float r = static_cast<float>(radius);

    for (int i = 0; i < 6; ++i) {
        glm::vec3 n(planes[i].x, planes[i].y, planes[i].z);
        float len = glm::length(n);
        if (len < 1e-6f) continue;

        n /= len;
        float d = planes[i].w / len;
        float dist = glm::dot(n, c) + d;
        if (dist < -r) {
            return false;
        }
    }

    return true;
}

}

Application::Application() : _window(nullptr) {}

Application::~Application() { cleanup(); }

void Application::setupQuad() {
    float quadVertices[] = {
        -1.0f,  1.0f,  0.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
    };

    glGenVertexArrays(1, &quadVAO);
    glGenBuffers(1, &quadVBO);
    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), &quadVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray(0);
}

void Application::recreateFBOs(int newWidth, int newHeight) {
    if (newWidth == _width && newHeight == _height) return;
    _width  = newWidth;
    _height = newHeight;

    if (_gBuffer)            _gBuffer            = std::make_unique<GBuffer>(_width, _height);
    if (_ssaoSystem)         _ssaoSystem         = std::make_unique<SSAO>(_width, _height);
    if (_hdrSystem)          _hdrSystem          = std::make_unique<HDR>(_width, _height);
    if (_bloomSystem)        _bloomSystem        = std::make_unique<Bloom>(_width, _height);
    if (_lightingTarget)     _lightingTarget     = std::make_unique<RenderTarget>(_width, _height);
    if (_bloomExtractTarget) _bloomExtractTarget = std::make_unique<RenderTarget>(_width, _height);
    if (_bloomPing)          _bloomPing          = std::make_unique<RenderTarget>(_width, _height);
    if (_bloomPong)          _bloomPong          = std::make_unique<RenderTarget>(_width, _height);
    if (_computePostProcess) {
        _computePostProcess = std::make_unique<ComputePostProcess>();
        _computePostProcess->init(_width, _height);
    }
}

void Application::loadScene(const std::string& scenePath) {
    _currentScene = std::make_unique<Haruka::Scene>();
    
    if (_currentScene->load(scenePath)) {
        std::cout << "Scene loaded: " << _currentScene->getName() << std::endl;
    } else {
        HARUKA_MOTOR_ERROR(ErrorCode::SCENE_PARSE_ERROR, std::string("Failed to load scene from: ") + scenePath);
        _currentScene = std::make_unique<Haruka::Scene>("DefaultScene");
    }
}

void Application::create_window() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        HARUKA_MOTOR_ERROR(ErrorCode::MOTOR_INIT_FAILED, "Failed to initialize SDL3");
        throw std::runtime_error("Fallo al inicializar SDL3");
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    _window = SDL_CreateWindow("Haruka Engine", _width, _height,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!_window) {
        HARUKA_MOTOR_ERROR(ErrorCode::WINDOW_CREATION_FAILED, "Failed to create SDL3 window");
        throw std::runtime_error("Fallo al crear la ventana");
    }
}

void Application::create_gl_context() {
    if (!_window) {
        HARUKA_MOTOR_ERROR(ErrorCode::OPENGL_INIT_FAILED, "create_gl_context() called without a window");
        throw std::runtime_error("create_gl_context() llamado sin ventana");
    }

    _glContext = SDL_GL_CreateContext(_window);
    if (!_glContext) {
        HARUKA_MOTOR_ERROR(ErrorCode::OPENGL_INIT_FAILED, "Failed to create OpenGL context via SDL3");
        throw std::runtime_error("Fallo al crear contexto OpenGL SDL3");
    }
    SDL_GL_MakeCurrent(_window, _glContext);
    SDL_GL_SetSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        HARUKA_MOTOR_ERROR(ErrorCode::OPENGL_INIT_FAILED, "Failed to initialize GLAD");
        throw std::runtime_error("Fallo al inicializar GLAD");
    }

    SDL_SetWindowRelativeMouseMode(_window, true);
    glEnable(GL_DEPTH_TEST);
}

void Application::init(Haruka::Scene& scene) {
    {
        static FILE* s_initLog = fopen("/tmp/haruka_render.log", "a");
        if (s_initLog) {
            fprintf(s_initLog, "[Application::init] scene='%s' objects=%zu\n",
                scene.getName().c_str(), scene.getObjects().size());
            fflush(s_initLog);
        }
    }

    // Resolver la ruta base al directorio del ejecutable para que los .spv
    // se encuentren independientemente del directorio de trabajo.
    Shader::setBaseDir(SDL_GetBasePath());

    _currentScene = std::make_unique<Haruka::Scene>(scene);
    // Usar siempre la cámara de la interfaz de juego si está disponible
    Camera* sceneCamera = nullptr;
    if (gameInterface && gameInterface->getCamera) {
        sceneCamera = gameInterface->getCamera();
        std::cout << (sceneCamera ? "✓ Camera from game interface" : "⚠ Game interface did not provide a camera") << std::endl;
    }

    auto applyCameraFromObj = [&](const Haruka::SceneObject& obj) {
        _camera = std::make_unique<Camera>(obj.position);
        glm::dquat qYaw   = glm::angleAxis(glm::radians(obj.rotation.y), glm::dvec3(0, 1, 0));
        glm::dquat qPitch = glm::angleAxis(glm::radians(obj.rotation.x), glm::dvec3(1, 0, 0));
        glm::dquat qRoll  = glm::angleAxis(glm::radians(obj.rotation.z), glm::dvec3(0, 0, 1));
        _camera->orientation = qYaw * qPitch * qRoll;
        if (obj.properties.is_object()) {
            if (obj.properties.contains("speed"))
                _camera->speed       = obj.properties["speed"].get<float>();
            if (obj.properties.contains("sensitivity"))
                _camera->sensitivity = obj.properties["sensitivity"].get<float>();
            if (obj.properties.contains("zoom"))
                _camera->zoom        = obj.properties["zoom"].get<float>();
        }
        std::cout << "✓ Camera from scene object: " << obj.name << std::endl;
    };

    if (sceneCamera) {
        _camera = std::make_unique<Camera>(sceneCamera->position);
        _camera->orientation = sceneCamera->orientation;
        _camera->zoom = sceneCamera->zoom;
        _camera->speed = sceneCamera->speed;
        _camera->sensitivity = sceneCamera->sensitivity;
    } else {
        // Search top-level objects then prefab children
        const Haruka::SceneObject* camObj = nullptr;
        for (const auto& obj : scene.getObjects()) {
            if (obj.type == "Camera") { camObj = &obj; break; }
            for (const auto& child : obj.children)
                if (child.type == "Camera") { camObj = &child; break; }
            if (camObj) break;
        }
        if (camObj)
            applyCameraFromObj(*camObj);
        else
            _camera = std::make_unique<Camera>(Haruka::WorldPos(0.0, 2.0, 8.0));
    }

    auto& rep = StartupReport::get();
    // Lambda: evita los problemas de comas en macros ya que el compilador
    // parsea los argumentos de funciones (no el preprocesador).
    auto tryInit = [&](const char* label, auto fn) {
        try { fn(); rep.record(label, StartupReport::OK); }
        catch (const std::exception& e) { rep.record(label, StartupReport::FAIL, e.what()); }
    };

    // ── Render systems ──────────────────────────────────────────────────────
    tryInit("Shadow",      [&]{ _shadowSystem      = std::make_unique<Shadow>(1024, 1024); });
    tryInit("HDR",         [&]{ _hdrSystem         = std::make_unique<HDR>(_width, _height); });
    tryInit("Bloom",       [&]{ _bloomSystem       = std::make_unique<Bloom>(_width, _height); });
    tryInit("GBuffer",     [&]{ _gBuffer           = std::make_unique<GBuffer>(_width, _height); });
    tryInit("SSAO",        [&]{ _ssaoSystem        = std::make_unique<SSAO>(_width, _height); });
    tryInit("IBL",         [&]{ _iblSystem         = std::make_unique<IBL>(); });
    tryInit("PointShadow", [&]{ _pointShadowSystem = std::make_unique<PointShadow>(1024); });
    tryInit("WorldSystem", [&]{ _worldSystem       = std::make_unique<Haruka::WorldSystem>(); });
    tryInit("LightCuller", [&]{ _lightCuller       = std::make_unique<LightCuller>(); });

    // ── Render targets ──────────────────────────────────────────────────────
    tryInit("RT:Lighting",     [&]{ _lightingTarget     = std::make_unique<RenderTarget>(_width, _height); });
    tryInit("RT:BloomExtract", [&]{ _bloomExtractTarget = std::make_unique<RenderTarget>(_width, _height); });
    tryInit("RT:BloomPing",    [&]{ _bloomPing          = std::make_unique<RenderTarget>(_width, _height); });
    tryInit("RT:BloomPong",    [&]{ _bloomPong          = std::make_unique<RenderTarget>(_width, _height); });

    if (!MotorInstance::getInstance().getRenderTarget()) {
        MotorInstance::getInstance().setRenderTarget(_lightingTarget.get());
    }
    MotorInstance::getInstance().setApplication(this);

    // ── Utility systems ─────────────────────────────────────────────────────
    tryInit("GPUInstancing", [&]{
        _instancing = std::make_unique<GPUInstancing>(GPUInstancing::PRECISION_FLOAT);
        _instancing->init(100000);
    });
    tryInit("AssetStreamer",  [&]{ AssetStreamer::getInstance().init(512, 2); });
    tryInit("DebugOverlay",   [&]{ DebugOverlay::getInstance().init(); });
    tryInit("ComputePostPro", [&]{
        _computePostProcess = std::make_unique<ComputePostProcess>();
        _computePostProcess->init(_width, _height);
    });
    tryInit("CascadedShadow", [&]{
        _cascadedShadow = std::make_unique<CascadedShadowMap>();
        _cascadedShadow->init(0.1f, 300000000000.0f, 0.75f);
    });
    tryInit("VirtualTexture", [&]{
        _virtualTexturing = std::make_unique<VirtualTexturing>();
        VTConfig vtConfig;
        vtConfig.pageSize        = 128;
        vtConfig.maxResidentPages = 1024;
        vtConfig.maxCacheMemory  = 256LL * 1024 * 1024;
        _virtualTexturing->init(vtConfig);
    });
    tryInit("PlanetarySystem", [&]{ _planetarySystem        = std::make_unique<Haruka::PlanetarySystem>(); });
    tryInit("RaycastSystem",   [&]{ _raycastSystem          = std::make_unique<RaycastSimple>(); });
    tryInit("TerrainStream",   [&]{ _terrainStreamingSystem = std::make_unique<Haruka::TerrainStreamingSystem>(); });
    tryInit("PhysicsEngine",   [&]{ _physicsEngine          = std::make_unique<Haruka::PhysicsEngine>(); });
    
    // Initialize chunk cache (256 MB default)
    _chunkCache = std::make_unique<Haruka::ChunkCache>(256);
    
    // Register terrain streaming system as observer
    if (_worldSystem && _terrainStreamingSystem) {
        _worldSystem->registerSystem(_terrainStreamingSystem.get());
        rep.record("RegisterTerrainStreaming", StartupReport::OK);
    }
    
    // Initialize planetary physics
    if (_physicsEngine && _worldSystem && _planetarySystem) {
        _physicsEngine->initPlanetaryPhysics(_worldSystem.get(), _planetarySystem.get(), _raycastSystem.get());
        rep.record("InitPlanetaryPhysics", StartupReport::OK);
    }

    setupQuad();
    rep.record("ScreenQuad", StartupReport::OK);

    // ── Primitives ───────────────────────────────────────────────────────────
    tryInit("TestCube", [&]{
        std::vector<glm::vec3> v, n;
        std::vector<unsigned int> idx;
        PrimitiveShapes::createCube(1.0f, v, n, idx);
        _testCube = std::make_unique<SimpleMesh>(v, n, idx);
    });

    int lodConfigs[4][2] = {{64,32},{32,16},{16,8},{8,4}};
    for (int i = 0; i < 4; i++) {
        std::vector<glm::vec3> v, n;
        std::vector<unsigned int> idx;
        PrimitiveShapes::createSphere(1.0f, lodConfigs[i][0], lodConfigs[i][1], v, n, idx);
        sphereLOD[i] = std::make_unique<SimpleMesh>(v, n, idx);
    }
    rep.record("SphereLODs", StartupReport::OK);

    tryInit("WorldComputeShaders", [&]{ _worldSystem->initComputeShaders(); });

    // ── Shaders ──────────────────────────────────────────────────────────────
    tryInit("Shader:main",        [&]{ _mainShader        = std::make_unique<Shader>("shaders/simple.vert",       "shaders/pbr.frag"); });
    tryInit("Shader:lamp",        [&]{ _lampShader        = std::make_unique<Shader>("shaders/simple.vert",       "shaders/light_cube.frag"); });
    tryInit("Shader:geom",        [&]{ _geomShader        = std::make_unique<Shader>("shaders/deferred_geom.vert","shaders/deferred_geom.frag"); });
    tryInit("Shader:ssao",        [&]{ _ssaoShader        = std::make_unique<Shader>("shaders/screenquad.vert",   "shaders/ssao.frag"); });
    tryInit("Shader:light",       [&]{ _lightShader       = std::make_unique<Shader>("shaders/screenquad.vert",   "shaders/deferred_light.frag"); });
    tryInit("Shader:composite",   [&]{ _compositeShader   = std::make_unique<Shader>("shaders/screenquad.vert",   "shaders/tonemapping.frag"); });
    tryInit("Shader:flat",        [&]{ _flatShader        = std::make_unique<Shader>("shaders/simple.vert",       "shaders/light_cube.frag"); });
    tryInit("Shader:cascade",     [&]{ _cascadeShadowShader = std::make_unique<Shader>("shaders/shadow.vert",     "shaders/shadow.frag"); });
    tryInit("Shader:bloomExtract",[&]{ _bloomExtractShader = std::make_unique<Shader>("shaders/screenquad.vert",  "shaders/bloom_extract.frag"); });
    tryInit("Shader:bloomBlur",   [&]{ _bloomBlurShader   = std::make_unique<Shader>("shaders/screenquad.vert",   "shaders/bloom_blur.frag"); });
    tryInit("Shader:pointShadow", [&]{ _pointShadowShader = std::make_unique<Shader>("shaders/point_shadow.vert", "shaders/point_shadow.frag", "shaders/point_shadow.geom"); });
    tryInit("Shader:instancing",  [&]{ _instancingShader  = std::make_unique<Shader>("shaders/instancing.vert",   "shaders/instancing.frag"); });

    // ── Scene mesh initialisation ────────────────────────────────────────────
    // Eagerly build primitives and warm model cache for layers 1-3 (always
    // resident). Layer 4-5 streaming objects are handled lazily in buildRenderQueue.
    for (auto& obj : _currentScene->getObjectsMutable()) {
        Haruka::ObjectType ot = Haruka::stringToObjectType(obj.type);
        if (!Haruka::isRenderableObjectType(ot)) continue;
        if (obj.renderLayer >= 4) continue; // handled by distance-based streaming
        initSceneObjectMesh(obj);
    }
    rep.record("SceneMeshInit", StartupReport::OK);

    rep.printSummary();
    _diagFramesLeft = 5;  // trigger per-init diagnostics for the next 5 frames
}

void Application::buildRenderQueue() {

    Haruka::Scene* scene = MotorInstance::getInstance().getScene();
    if (!scene) {
        scene = _currentScene.get();
    }
    g_sceneForRender = scene;
    g_sceneRenderQueue.clear();
    if (!scene) return;

    Camera* activeCamera = MotorInstance::getInstance().getCamera();
    if (!activeCamera) {
        activeCamera = _camera.get();
    }

    // Dynamic near plane: scale with altitude above planet surface to reduce
    // the far/near ratio, which directly controls depth-buffer precision.
    // On the surface near stays 0.1 m (10 cm); at 400 km orbit near ≈ 400 m,
    // giving a ratio of ~7.5e8 instead of 3e12 (dramatically less z-fighting).
    double nearPlane = 0.1;
    const double farPlane = 300000000000.0;  // 300 Gm (~2 AU)
    if (activeCamera && scene) {
        for (const auto& obj : scene->getObjects()) {
            if (!obj.properties.is_object() || !obj.properties.contains("terrainEditor")) continue;
            if (!obj.properties["terrainEditor"].value("isPlanetRoot", false)) continue;
            glm::dvec3 planetCenter = obj.getWorldPosition(scene);
            double planetRadius = std::max({std::abs(obj.scale.x), std::abs(obj.scale.y), std::abs(obj.scale.z)});
            if (planetRadius < 1.0) break;
            double altitude = std::max(0.0, glm::length(activeCamera->position - planetCenter) - planetRadius);
            nearPlane = std::max(0.1, altitude * 0.001);
            break;
        }
    }

    const double aspect = (_height > 0) ? static_cast<double>(_width) / static_cast<double>(_height) : (16.0 / 9.0);

    glm::dvec3 camPos(0.0);
    glm::mat4 viewProj(1.0f);
    bool canCullByFrustum = false;

    if (activeCamera) {
        camPos = activeCamera->position;
        glm::mat4 proj = glm::perspective(glm::radians(activeCamera->zoom), static_cast<float>(aspect), static_cast<float>(nearPlane), static_cast<float>(farPlane));
        glm::mat4 view = activeCamera->getViewMatrix();
        // Strip translation from view matrix so frustum planes are in camera-relative
        // space. At world coordinates ~1.5e11 m, float32 ULP is ~16 km and a straight
        // proj*view culling test causes catastrophic cancellation → chunks rejected.
        glm::mat4 viewRot = glm::mat4(glm::mat3(view));
        viewProj = proj * viewRot;
        canCullByFrustum = true;
    }

    Haruka::TerrainStreamingStats terrainStats;
    if (_terrainStreamingSystem) {
        _terrainStreamingSystem->update(scene,
                                        _worldSystem.get(),
                                        _planetarySystem.get(),
                                        _raycastSystem.get(),
                                        activeCamera,
                                        &terrainStats);
    }

    _iVisibleChunks         = terrainStats.visibleChunks;
    _iResidentChunks        = terrainStats.residentChunks;
    _iPendingChunkLoads     = terrainStats.pendingChunkLoads;
    _iPendingChunkEvictions = terrainStats.pendingChunkEvictions;
    _iResidentMemoryMB      = terrainStats.residentMemoryMB;
    _iTrackedChunks         = terrainStats.trackedChunks;
    _iMaxMemoryMB           = terrainStats.maxMemoryMB;

    // Compute camera direction relative to planet center for correct hemisphere culling.
    glm::vec3 camDirFromPlanet(0.0f, 0.0f, 1.0f);
    {
        glm::dvec3 pc = _worldSystem ? _worldSystem->getPlanetCenter() : glm::dvec3(0.0);
        glm::dvec3 rel = camPos - pc;
        double rlen = glm::length(rel);
        if (rlen > 1e-6) camDirFromPlanet = glm::vec3(rel / rlen);
    }

    for (auto& obj : scene->getObjectsMutable()) {
        if (isRenderDisabledByEditor(obj)) continue;
        if (!isTerrainChunkFacingCamera(obj, camDirFromPlanet)) continue;
        Haruka::ObjectType objType = Haruka::stringToObjectType(obj.type);
        if (!Haruka::isRenderableObjectType(objType)) continue;

        int layer = std::clamp(obj.renderLayer, 1, 5);
        const glm::dvec3& worldPos = obj.getWorldPosition(scene);
        const glm::dvec3& worldScale = obj.getWorldScale(scene);
        double maxScale = std::max(std::abs(worldScale.x), std::max(std::abs(worldScale.y), std::abs(worldScale.z)));
        // isPlanetRoot objects are managed by the terrain streaming system (they get
        // their base mesh released once chunks are resident). Keep them in the render
        // queue unconditionally so the base sphere is visible before chunks arrive.
        const bool isPlanetRoot = obj.properties.is_object()
            && obj.properties.contains("terrainEditor")
            && obj.properties["terrainEditor"].value("isPlanetRoot", false);
        const bool isHugeBody = isPlanetRoot || (maxScale > 1000.0) || (obj.name == "Earth") || (obj.name == "Sun");

        // Cuerpos gigantes (Earth/Sun) nunca se descargan ni se reconstruyen
        if (!isHugeBody) {
            double unloadDistance = static_cast<double>(s_layerMaxDistance[layer]);
            double loadDistance = unloadDistance * 0.85;

            if (layer >= 4 && obj.meshRenderer) {
                glm::dvec3 diff = worldPos - camPos;
                double distSq = glm::dot(diff, diff);
                double unloadDistSq = unloadDistance * unloadDistance * 1.32;  // (1.15)² ≈ 1.32
                double loadDistanceSq = loadDistance * loadDistance;
                if (distSq > unloadDistSq) {
                    maybeReleasePrimitiveMesh(obj);
                } else if (!obj.meshRenderer->isResident() && distSq < loadDistanceSq) {
                    buildPrimitiveMeshFromProperties(obj);
                }
            }

            if (layer >= 4 && !obj.modelPath.empty()) {
                double distSq = glm::dot(worldPos - camPos, worldPos - camPos);
                double unloadDistSq = unloadDistance * unloadDistance * 1.32;
                if (distSq > unloadDistSq) {
                    releaseModelFromCache(obj.modelPath);
                }
            }
        }

        // Cuerpos gigantes siempre se renderizan sin culling.
        // Build their primitive mesh on demand if not yet resident (the normal
        // distance-based lifecycle skips isHugeBody objects, so we must do it here).
        if (isHugeBody) {
            if (!obj.meshRenderer) {
                obj.meshRenderer = std::make_shared<MeshRendererComponent>();
            }
            if (!obj.meshRenderer->isResident()) {
                // Try structured property path first.
                buildPrimitiveMeshFromProperties(obj);
                // If still no mesh (no meshRenderer property), generate a unit sphere fallback.
                if (!obj.meshRenderer->isResident()) {
                    std::vector<glm::vec3> v, n;
                    std::vector<unsigned int> idx;
                    PrimitiveShapes::createSphere(1.0f, 32, 32, v, n, idx);
                    obj.meshRenderer->setMesh(v, n, idx);
                }
            }
            g_sceneRenderQueue.push_back(&obj);
            continue;
        }

        // Para objetos normales, aplicar culling por frustum y distancia
        if (canCullByFrustum) {
            double radius = std::max(0.5 * glm::length(worldScale), maxScale);
            if (radius < 0.001) {
                radius = 0.5;
            }

            if (layer != 1) {
                static constexpr double qualityMul[4] = {0.45, 0.70, 1.00, 1.35};
                glm::dvec3 diff = worldPos - camPos;
                double distSq = glm::dot(diff, diff);
                double maxDist = static_cast<double>(s_layerMaxDistance[layer]) * qualityMul[s_renderQualityPreset];
                double radiusPlus = radius + maxDist;
                if (std::sqrt(distSq) > radiusPlus) {
                    continue;
                }
            }

            if (!isSphereInsideCameraFrustum(
                    worldPos - camPos,
                    radius,
                    viewProj)) {
                continue;
            }
        }

        g_sceneRenderQueue.push_back(&obj);
    }

    _iTotalVertices = _iTotalTriangles = _iTotalDrawCalls = 0;
    for (const auto& obj : scene->getObjects()) {
        if (isRenderDisabledByEditor(obj)) continue;
        Haruka::ObjectType objType = Haruka::stringToObjectType(obj.type);
        if (!Haruka::isRenderableObjectType(objType)) continue;
        if (obj.meshRenderer) {
            _iTotalDrawCalls++;
            _iTotalVertices  += obj.meshRenderer->getVertexCount();
            _iTotalTriangles += obj.meshRenderer->getTriangleCount();
        } else if (!obj.modelPath.empty()) {
            try {
                auto model = getOrLoadModel(obj.modelPath);
                if (!model) continue;
                _iTotalDrawCalls++;
                _iTotalVertices  += model->getVertexCount();
                _iTotalTriangles += model->getTriangleCount();
            } catch (...) {}
        }
    }

    _iRenderedVertices = _iRenderedTriangles = _iRenderedDrawCalls = 0;
    for (const auto* obj : g_sceneRenderQueue) {
        if (!obj) continue;
        if (obj->meshRenderer && obj->meshRenderer->isResident()) {
            _iRenderedDrawCalls++;
            _iRenderedVertices  += obj->meshRenderer->getResidentVertexCount();
            _iRenderedTriangles += obj->meshRenderer->getResidentTriangleCount();
        } else if (!obj->modelPath.empty()) {
            try {
                auto model = getOrLoadModel(obj->modelPath);
                if (!model) continue;
                _iRenderedDrawCalls++;
                _iRenderedVertices  += model->getVertexCount();
                _iRenderedTriangles += model->getTriangleCount();
            } catch (...) {}
        }
    }
}

void Application::main_loop() {
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
                if (_camera) {
                    _camera->rotate(static_cast<float>(event.motion.xrel),
                                    -static_cast<float>(event.motion.yrel));
                }
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                if (_camera) _camera->ProcessMouseScroll(static_cast<float>(event.wheel.y));
            } else if (event.type == SDL_EVENT_WINDOW_RESIZED) {
                SDL_GetWindowSize(_window, &_width, &_height);
            }
        }
        renderFrame();
        SDL_GL_SwapWindow(_window);
    }
}

void Application::renderFrame() {
    static FILE* s_log = nullptr;
    if (!s_log) s_log = fopen("/tmp/haruka_render.log", "w");

    Haruka::Scene* scene = MotorInstance::getInstance().getScene();
    if (!scene && !_currentScene) {
        if (s_log) { fprintf(s_log, "renderFrame: early exit — no scene\n"); fflush(s_log); }
        return;
    }
    if (!_camera) {
        if (s_log) { fprintf(s_log, "renderFrame: early exit — no _camera\n"); fflush(s_log); }
        return;
    }

    static int s_frameCount = 0;
    if (++s_frameCount <= 3 && s_log) {
        fprintf(s_log, "renderFrame #%d: scene=%s camera=(%g,%g,%g) motorRT=%p\n",
            s_frameCount,
            scene ? scene->getName().c_str() : (_currentScene ? _currentScene->getName().c_str() : "null"),
            (double)_camera->position.x, (double)_camera->position.y, (double)_camera->position.z,
            (void*)MotorInstance::getInstance().getRenderTarget());
        fflush(s_log);
    }

    _frameStart = std::chrono::high_resolution_clock::now();

    renderFrameContent();

    auto frameEnd = std::chrono::high_resolution_clock::now();
    _lastFrameTimeMs = std::chrono::duration<float, std::milli>(frameEnd - _frameStart).count();
    deltaTime        = _lastFrameTimeMs * 0.001f;

    _fpsFrameCount++;
    double now = std::chrono::duration<double>(frameEnd.time_since_epoch()).count();
    if (now - _fpsLastTime >= 1.0) {
        _lastFps      = static_cast<float>(_fpsFrameCount / (now - _fpsLastTime));
        _fpsLastTime  = now;
        _fpsFrameCount = 0;
    }
}

void Application::renderFrameContent() {
    static FILE* s_log2 = fopen("/tmp/haruka_render.log", "a");
    // ========== SETUP ==========
    Camera* activeCamera = MotorInstance::getInstance().getCamera();
    if (!activeCamera) {
        activeCamera = _camera.get();
    }
    if (!activeCamera) {
        if (s_log2) { fprintf(s_log2, "renderFrameContent: no activeCamera\n"); fflush(s_log2); }
        return;
    }

    glm::mat4 proj = glm::perspective(glm::radians(activeCamera->zoom), (float)_width / (float)_height, 0.1f, 300000000000.0f);
    glm::mat4 view = activeCamera->getViewMatrix();

    // Camera direction relative to planet center for correct hemisphere culling.
    glm::vec3 camDirFromPlanet(0.0f, 0.0f, 1.0f);
    {
        glm::dvec3 pc = _worldSystem ? _worldSystem->getPlanetCenter() : glm::dvec3(0.0);
        glm::dvec3 rel = glm::dvec3(activeCamera->position) - pc;
        double rlen = glm::length(rel);
        if (rlen > 1e-6) camDirFromPlanet = glm::vec3(rel / rlen);
    }

    // ========== EDITOR MODE (Viewport) ==========
    // Use _editorTarget set directly by the viewport (bypasses MotorInstance singleton split
    // between the engine lib and the editor executable). Fall back to MotorInstance only when
    // the engine is running standalone (no editor target set).
    RenderTarget* editorTarget = _editorTarget
        ? _editorTarget
        : MotorInstance::getInstance().getRenderTarget();
    {
        static bool s_pathLogged = false;
        if (!s_pathLogged) {
            s_pathLogged = true;
            if (s_log2) {
                fprintf(s_log2,
                    "renderFrameContent: _editorTarget=%p motorRT=%p using=%p isPlayMode=%d\n",
                    (void*)_editorTarget,
                    (void*)MotorInstance::getInstance().getRenderTarget(),
                    (void*)editorTarget,
                    (int)MotorInstance::getInstance().isPlayMode());
                fflush(s_log2);
            }
        }
    }
    if (editorTarget && !MotorInstance::getInstance().isPlayMode()) {
        buildRenderQueue();

        // ── PER-INIT DIAGNOSTICS: fire for first 5 frames after each init() ──
        if (_diagFramesLeft > 0) {
            --_diagFramesLeft;
            GLint linked = 0;
            if (_flatShader) glGetProgramiv(_flatShader->ID, GL_LINK_STATUS, &linked);
            editorTarget->bindForWriting();
            GLenum fbStatus = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);

            Haruka::Scene* dbgScene = MotorInstance::getInstance().getScene();
            if (!dbgScene) dbgScene = _currentScene.get();

            char buf[512];
            snprintf(buf, sizeof(buf),
                "[RenderDiag#%d] scene=%s flatShader.ID=%u linked=%d"
                " fbStatus=0x%X queueSize=%zu _editorTarget=%p\n",
                5 - _diagFramesLeft,
                dbgScene ? dbgScene->getName().c_str() : "null",
                _flatShader ? _flatShader->ID : 0u, linked,
                fbStatus, g_sceneRenderQueue.size(),
                (void*)_editorTarget);
            if (s_log2) { fprintf(s_log2, "%s", buf); fflush(s_log2); }

            int shown = 0;
            for (const auto* o : g_sceneRenderQueue) {
                if (!o || shown++ >= 6) break;
                bool res   = o->meshRenderer && o->meshRenderer->isResident();
                int  verts = res ? o->meshRenderer->getResidentVertexCount() : 0;
                snprintf(buf, sizeof(buf),
                    "[RenderDiag]   \"%s\" type=%s resident=%d verts=%d\n",
                    o->name.c_str(), o->type.c_str(), (int)res, verts);
                if (s_log2) { fprintf(s_log2, "%s", buf); fflush(s_log2); }
            }
            if (g_sceneRenderQueue.empty() && s_log2) {
                fprintf(s_log2, "[RenderDiag]   (queue empty)\n");
                fflush(s_log2);
            }
        }
        // ─────────────────────────────────────────────────────────────────────

        editorTarget->bindForWriting();
        glEnable(GL_DEPTH_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // simple.vert layout locations:  model=0, view=1, projection=2
        // light_cube.frag layout locs:   lightColor=3, sunDirection=4, sunLightColor=5,
        //                                ambientStrength=6, useProceduralTerrain=7
        //                                planetCenter=8, planetRadius=9
        //
        // Camera-relative rendering: strip translation from the view matrix and
        // inject a camera-relative translation into each model matrix instead.
        // The subtraction (objWorldPos - camPos) is done in double precision so
        // float32 never sees the large absolute world coordinates (~149M units).
        glm::dvec3 camPosD = glm::dvec3(activeCamera->position);
        glm::mat4 viewRot  = glm::mat4(glm::mat3(view)); // rotation only, no translation

        _flatShader->use();
        _flatShader->setMat4(2, proj);    // projection
        _flatShader->setMat4(1, viewRot); // view (rotation-only)

        glm::vec3 sunDir = glm::normalize(glm::vec3(0.3f, 0.6f, 0.7f));
        glm::vec3 sunLightColor = glm::vec3(1.0f);
        for (const auto* obj : g_sceneRenderQueue) {
            if (!obj) continue;
            if (obj->type == "Light" || obj->type == "PointLight" || obj->type == "DirectionalLight") {
                // Sun direction = from camera toward sun (camera-relative space)
                glm::dvec3 sunWorldPos = obj->getWorldPosition(g_sceneForRender);
                glm::vec3 toSun = glm::vec3(sunWorldPos - camPosD);
                if (glm::length(toSun) > 0.001f) sunDir = glm::normalize(toSun);
                float sunEnergy = std::clamp(std::max((float)obj->intensity, 0.0f) * 0.01f, 0.2f, 2.0f);
                sunLightColor = glm::vec3(obj->color) * sunEnergy;
                break;
            }
        }
        _flatShader->setVec3(4, sunDir);         // sunDirection
        _flatShader->setVec3(5, sunLightColor);  // sunLightColor
        _flatShader->setFloat(6, 0.12f);         // ambientStrength

        for (const auto* obj : g_sceneRenderQueue) {
            if (!obj) continue;
            // Build camera-relative model matrix:
            // keep rotation+scale from the world transform, replace translation
            // with high-precision (double) offset from camera.
            glm::mat4 worldTransform = g_sceneForRender
                ? obj->getWorldTransform(g_sceneForRender) : glm::mat4(1.0f);
            glm::dvec3 objWorldPos = g_sceneForRender
                ? obj->getWorldPosition(g_sceneForRender) : glm::dvec3(0.0);
            glm::vec3 relTrans = glm::vec3(objWorldPos - camPosD);
            worldTransform[3] = glm::vec4(relTrans, 1.0f);

            _flatShader->setMat4(0, worldTransform); // model (camera-relative)
            glm::vec3 baseColor = glm::vec3(obj->color);
            if (glm::length(baseColor) < 0.001f) baseColor = glm::vec3(0.8f, 0.8f, 0.8f);
            const bool isLightObj = (obj->type == "Light" || obj->type == "PointLight" || obj->type == "DirectionalLight");
            float emission = isLightObj ? std::max((float)obj->intensity, 0.0f) : 1.0f;
            glm::vec3 c = isLightObj ? (baseColor * emission) : baseColor;
            _flatShader->setVec3(3, c);           // lightColor
            const bool useProc = !isLightObj && shouldUseProceduralTerrainLook(*obj);
            _flatShader->setBool(7, useProc);     // useProceduralTerrain
            if (useProc) {
                // planetCenter and planetRadius in camera-relative space.
                // Use world-space radius so terrain chunks (scale=1.0 locally,
                // but parented to Earth at scale=6371000) get the right radius.
                float worldRadius = g_sceneForRender
                    ? (float)obj->getWorldScale(g_sceneForRender).x
                    : (float)obj->scale.x;
                _flatShader->setVec3(8, relTrans);
                _flatShader->setFloat(9, worldRadius);
            }
            if (obj->meshRenderer && obj->meshRenderer->isResident()) {
                obj->meshRenderer->render(*_flatShader);
                continue;
            }
            if (!obj->modelPath.empty()) {
                try {
                    auto model = getOrLoadModel(obj->modelPath);
                    if (model) model->Draw(*_flatShader);
                } catch (...) {}
                continue;
            }
        }

        if (_imguiCallback) _imguiCallback();

        editorTarget->unbind();
        return;
    }

    // ========== RUNTIME MODE (Deferred Pipeline) ==========
    
    // Actualizar posición de cámara
    AssetStreamer::getInstance().updateCameraPosition(activeCamera->position);
    Haruka::Scene* shadowScene = MotorInstance::getInstance().getScene();
    if (!shadowScene) shadowScene = _currentScene.get();
    
    // Actualizar cascadas de sombra
    glm::vec3 lightDir = glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f));
    if (shadowScene) {
        for (const auto& obj : shadowScene->getObjects()) {
            if (obj.type == "Light" || obj.type == "DirectionalLight") {
                glm::vec3 sunPos = glm::vec3(obj.getWorldPosition(shadowScene));
                if (glm::length(sunPos) > 1e-6f) {
                    lightDir = glm::normalize(-sunPos);
                }
                break;
            }
        }
    }
    glm::vec3 camForward = activeCamera->getFront();
    glm::vec3 camUp = activeCamera->getUp();
    _cascadedShadow->updateCascades(
        lightDir,
        activeCamera->position,
        camForward,
        camUp,
        (float)_width / (float)_height,
        0.1f,
        300000000000.0f,
        75.0f
    );

    if (_cascadedShadow && _cascadeShadowShader && shadowScene) {
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.5f, 4.0f);

        _cascadeShadowShader->use();
        for (int cascade = 0; cascade < _cascadedShadow->getNumCascades(); ++cascade) {
            _cascadedShadow->bindForWriting(cascade);
            glClear(GL_DEPTH_BUFFER_BIT);

            _cascadeShadowShader->setMat4("lightSpaceMatrix", _cascadedShadow->getCascadeMatrix(cascade));

            for (const auto& obj : shadowScene->getObjects()) {
                if (isRenderDisabledByEditor(obj)) continue;
                if (!isTerrainChunkFacingCamera(obj, camDirFromPlanet)) continue;
                glm::mat4 modelMatrix = obj.getWorldTransform(shadowScene);
                _cascadeShadowShader->setMat4("model", modelMatrix);

                if (obj.meshRenderer && obj.meshRenderer->isResident()) {
                    obj.meshRenderer->render(*_cascadeShadowShader);
                } else if (!obj.modelPath.empty()) {
                    auto model = getOrLoadModel(obj.modelPath);
                    if (model) model->Draw(*_cascadeShadowShader);
                }
            }

            if (_worldSystem) {
                for (const auto& body : _worldSystem->getBodies()) {
                    if (!body.visible) continue;
                    if (shadowScene && shadowScene->getObject(body.name)) continue;
                    int lod = 0;
                    glm::vec3 camPos(activeCamera->position.x, activeCamera->position.y, activeCamera->position.z);
                    glm::vec3 bodyPos(body.localPos.x, body.localPos.y, body.localPos.z);
                    float distance = glm::length(camPos - bodyPos);
                    lod = distance < 50.0f ? 0 : distance < 200.0f ? 1 : distance < 1000.0f ? 2 : 3;
                    if (!sphereLOD[lod]) continue;

                    glm::mat4 bodyModel = glm::translate(glm::mat4(1.0f), glm::vec3(body.localPos));
                    bodyModel = glm::scale(bodyModel, glm::vec3(body.radius));
                    _cascadeShadowShader->setMat4("model", bodyModel);
                    sphereLOD[lod]->draw();
                }
            }
        }

        glCullFace(GL_BACK);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, _width, _height);
    }

    // ========== POINT SHADOW DEPTH PASS ==========
    if (_pointShadowSystem && _pointShadowShader && shadowScene) {
        // Pick the first point light as the shadow caster
        glm::vec3 pointLightPos(0.0f);
        bool foundPointLight = false;
        for (const auto& obj : shadowScene->getObjects()) {
            if (obj.type == "PointLight" || obj.type == "Light") {
                glm::mat4 t = obj.getWorldTransform(shadowScene);
                pointLightPos = glm::vec3(t[3]);
                foundPointLight = true;
                break;
            }
        }

        if (foundPointLight) {
            const float psNear = 0.1f, psFar = 25.0f;
            glm::mat4 shadowProj = glm::perspective(glm::radians(90.0f), 1.0f, psNear, psFar);
            glm::mat4 shadowMatrices[6] = {
                shadowProj * glm::lookAt(pointLightPos, pointLightPos + glm::vec3( 1, 0, 0), glm::vec3(0,-1, 0)),
                shadowProj * glm::lookAt(pointLightPos, pointLightPos + glm::vec3(-1, 0, 0), glm::vec3(0,-1, 0)),
                shadowProj * glm::lookAt(pointLightPos, pointLightPos + glm::vec3( 0, 1, 0), glm::vec3(0, 0, 1)),
                shadowProj * glm::lookAt(pointLightPos, pointLightPos + glm::vec3( 0,-1, 0), glm::vec3(0, 0,-1)),
                shadowProj * glm::lookAt(pointLightPos, pointLightPos + glm::vec3( 0, 0, 1), glm::vec3(0,-1, 0)),
                shadowProj * glm::lookAt(pointLightPos, pointLightPos + glm::vec3( 0, 0,-1), glm::vec3(0,-1, 0)),
            };

            _pointShadowSystem->bindForWriting();
            _pointShadowShader->use();
            for (int f = 0; f < 6; ++f)
                _pointShadowShader->setMat4("shadowMatrices[" + std::to_string(f) + "]", shadowMatrices[f]);
            _pointShadowShader->setVec3("lightPos", pointLightPos);
            _pointShadowShader->setFloat("farPlane", psFar);

            for (const auto& obj : shadowScene->getObjects()) {
                if (isRenderDisabledByEditor(obj)) continue;
                _pointShadowShader->setMat4("model", obj.getWorldTransform(shadowScene));
                if (obj.meshRenderer && obj.meshRenderer->isResident())
                    obj.meshRenderer->render(*_pointShadowShader);
                else if (!obj.modelPath.empty()) {
                    auto mdl = getOrLoadModel(obj.modelPath);
                    if (mdl) mdl->Draw(*_pointShadowShader);
                }
            }

            _pointShadowSystem->unbind();
            glViewport(0, 0, _width, _height);
        }
    }

    // Procesar feedback de virtual texturing
    if (_virtualTexturing) {
        _virtualTexturing->processFeedback();
    }

    // ========== GEOMETRY PASS ==========
    _gBuffer->bindForWriting();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    _geomShader->use();
    _geomShader->setMat4("projection", proj);
    _geomShader->setMat4("view", view);

    buildRenderQueue();

    int drawCount = 0;
    for (const auto* obj : g_sceneRenderQueue) {
        if (!obj) continue;
        glm::mat4 modelMatrix = g_sceneForRender ? obj->getWorldTransform(g_sceneForRender) : glm::mat4(1.0f);
        _geomShader->setMat4("model", modelMatrix);
        _geomShader->setVec3("color", glm::vec3(obj->color));
        if (obj->meshRenderer && obj->meshRenderer->isResident()) {
            obj->meshRenderer->render(*_geomShader);
            drawCount++;
            continue;
        }
        if (!obj->modelPath.empty()) {
            try {
                auto model = getOrLoadModel(obj->modelPath);
                if (model) model->Draw(*_geomShader);
                drawCount++;
            } catch (const std::exception& e) {
                HARUKA_MOTOR_ERROR(
                    ErrorCode::MODEL_LOAD_FAILED,
                    std::string("Failed to draw model ") + obj->modelPath + ": " + e.what()
                );
            }
            continue;
        }
    }
    // Render celestial bodies via GPU instancing (all into GBuffer in one draw call)
    if (_instancing && _instancingShader && _worldSystem && sphereLOD[0]) {
        _instancing->clear();
        for (const auto& body : _worldSystem->getBodies()) {
            if (!body.visible) continue;
            if (g_sceneForRender && g_sceneForRender->getObject(body.name)) continue;
            glm::vec3 bodyPos(body.localPos.x, body.localPos.y, body.localPos.z);
            _instancing->addInstanceFloat(
                bodyPos,
                glm::vec3(body.radius),
                glm::vec4(body.color, 1.0f)
            );
        }
        if (_instancing->getInstanceCount() > 0) {
            _instancingShader->use();
            _instancingShader->setMat4("view", view);
            _instancingShader->setMat4("projection", proj);
            _instancing->render(sphereLOD[0]->VAO,
                                (GLuint)sphereLOD[0]->getIndexCount());
        }
    }
        
    // Hardcoded test cube at world origin — remove once pipeline is verified
    if (_testCube) {
        _geomShader->use();
        _geomShader->setMat4("model", glm::mat4(1.0f));
        _geomShader->setVec3("color", glm::vec3(0.8f, 0.3f, 0.1f));
        _geomShader->setVec3("emissiveFallback", glm::vec3(0.8f, 0.3f, 0.1f));
        _testCube->draw();
        _geomShader->setVec3("emissiveFallback", glm::vec3(0.0f)); // reset for other objects
    }

    _gBuffer->unbind();

    // ========== SSAO PASS ==========
    _ssaoSystem->bindForWriting();
    glClear(GL_COLOR_BUFFER_BIT);

    _ssaoShader->use();
    _gBuffer->bindForReading(0, 2);  // gPosition → unit 2  (binding=2)
    _gBuffer->bindForReading(1, 3);  // gNormal   → unit 3  (binding=3)
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, _ssaoSystem->getNoiseTexture()); // texNoise → unit 4 (binding=4)

    const auto& kernel = _ssaoSystem->getKernel();
    for (int i = 0; i < (int)kernel.size(); ++i)
        _ssaoShader->setVec3("samples[" + std::to_string(i) + "]", kernel[i]);
    _ssaoShader->setInt("kernelSize", (int)kernel.size());
    _ssaoShader->setFloat("radius", 0.5f);
    _ssaoShader->setFloat("bias", 0.025f);
    _ssaoShader->setMat4("projection", proj);

    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    _ssaoSystem->unbind();

    // ========== LIGHTING PASS ==========
    _hdrSystem->bindForWriting();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    _lightShader->use();

    _gBuffer->bindForReading(0, 0);
    _gBuffer->bindForReading(1, 1);
    _gBuffer->bindForReading(2, 2);
    _gBuffer->bindForReading(3, 3);
    _ssaoSystem->bindForReading(4);
    _iblSystem->bindPrefilterMap(5);
    _iblSystem->bindBRDFLUT(6);
    _pointShadowSystem->bindForReading(11);

    _lightShader->setInt("gPosition", 0);
    _lightShader->setInt("gNormal", 1);
    _lightShader->setInt("gAlbedoSpec", 2);
    _lightShader->setInt("gEmissive", 3);
    _lightShader->setInt("ssao", 4);
    _lightShader->setInt("prefilterMap", 5);
    _lightShader->setInt("brdfLUT", 6);
    _lightShader->setFloat("pointFarPlane", 25.0f);
    _lightShader->setVec3("viewPos", activeCamera->position);
    _lightShader->setMat4("view", view);
    _lightShader->setInt("numCascades", _cascadedShadow ? _cascadedShadow->getNumCascades() : 0);
    if (_cascadedShadow) {
        for (int i = 0; i < _cascadedShadow->getNumCascades(); ++i) {
            _lightShader->setMat4("cascadeLightSpaceMatrices[" + std::to_string(i) + "]", _cascadedShadow->getCascadeMatrix(i));
            _lightShader->setFloat("cascadeSplits[" + std::to_string(i) + "]", _cascadedShadow->getCascadeInfo(i).zFar);
            _cascadedShadow->bindForReading(i, 7 + i);
            _lightShader->setInt("cascadeShadowMaps[" + std::to_string(i) + "]", 7 + i);
        }
    }

    // Light culling
    Haruka::Scene* scene = MotorInstance::getInstance().getScene();
    if (!scene) {
        scene = _currentScene.get();
    }
    auto culledLights = _lightCuller->cullLights(scene, view, proj, MAX_LIGHTS);
    
    _lightShader->setInt("numLights", (int)culledLights.size());
    for (size_t i = 0; i < culledLights.size(); i++) {
        _lightShader->setVec3("lightPositions[" + std::to_string(i) + "]", culledLights[i].position);
        _lightShader->setVec3("lightColors[" + std::to_string(i) + "]", culledLights[i].color);
    }

    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    _hdrSystem->unbind();

    // ========== BLOOM EXTRACT ==========
    _bloomExtractTarget->bindForWriting();
    glClear(GL_COLOR_BUFFER_BIT);
    _bloomExtractShader->use();
    _hdrSystem->bindForReading(0, 0);  // colorTexture → unit 0
    _bloomExtractShader->setFloat("threshold", 1.0f);
    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    _bloomExtractTarget->unbind();

    // ========== BLOOM BLUR (ping-pong, 10 passes) ==========
    _bloomBlurShader->use();
    RenderTarget* blurSrc = _bloomExtractTarget.get();
    RenderTarget* pingpong[2] = { _bloomPing.get(), _bloomPong.get() };
    bool horizontal = true;
    for (int i = 0; i < 10; ++i) {
        int dst = horizontal ? 0 : 1;
        pingpong[dst]->bindForWriting();
        glClear(GL_COLOR_BUFFER_BIT);
        blurSrc->bindForReading(0);
        _bloomBlurShader->setBool("horizontal", horizontal);
        glBindVertexArray(quadVAO);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        pingpong[dst]->unbind();
        blurSrc = pingpong[dst];
        horizontal = !horizontal;
    }
    // After 10 iters (even), last write → pingpong[1] = _bloomPong

    // ========== FINAL COMPOSITE ==========
    RenderTarget* externalTarget = _editorTarget
        ? _editorTarget
        : MotorInstance::getInstance().getRenderTarget();
    if (externalTarget) {
        externalTarget->bindForWriting();
    }
    glViewport(0, 0, _width, _height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    _compositeShader->use();
    _hdrSystem->bindForReading(1, 0);  // colorTexture → unit 1 (binding=1)
    _bloomPong->bindForReading(2);     // bloom        → unit 2 (binding=2)
    _compositeShader->setFloat("exposure", _exposure);
    _compositeShader->setFloat("bloomStrength", 0.8f);

    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    if (externalTarget) {
        externalTarget->unbind();
    }

    // ========== COMPUTE COLOR GRADING ==========
    if (_computePostProcess && _lightingTarget) {
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        _computePostProcess->colorGradingCompute(
            _lightingTarget->getColorTexture(),
            1.1f,   // saturation
            1.05f,  // contrast
            0.0f    // brightness
        );
    }

    // ========== DEBUG METRICS ==========
    FrameMetrics metrics;
    metrics.fps         = _lastFps;
    metrics.frameTimeMs = _lastFrameTimeMs;
    metrics.drawCalls = 0;
    metrics.totalTriangles = 0;
    metrics.totalVertices = 0;
    metrics.totalLights = scene ? scene->getObjects().size() : 0;
    metrics.culledLights = _lightCuller ? _lightCuller->getCulledLights() : 0;

    auto streamStats = AssetStreamer::getInstance().getStats();
    metrics.loadedAssets = streamStats.loadedAssets;
    metrics.pendingAssets = streamStats.pendingAssets;
    metrics.cacheUtilization = streamStats.cacheUtilization;

    metrics.numCascades = 4;
    metrics.activeCascade = 0;

    // Recorrer la cola de render y sumar vértices/triángulos/draw calls
    for (const auto* item : g_sceneRenderQueue) {
        if (!item) continue;
        if (item->meshRenderer) {
            // Suponiendo que meshRenderer tiene métodos para obtener stats
            metrics.drawCalls++;
            metrics.totalVertices += item->meshRenderer->getVertexCount();
            metrics.totalTriangles += item->meshRenderer->getTriangleCount();
        } else if (!item->modelPath.empty()) {
            try {
                Model model(item->modelPath);
                metrics.drawCalls++;
                metrics.totalVertices += model.getVertexCount();
                metrics.totalTriangles += model.getTriangleCount();
            } catch (...) {}
        }
    }

    DebugOverlay::getInstance().updateMetrics(metrics);
}

void Application::cleanup() {
    MotorInstance::getInstance().clear();

    // Render systems (orden inverso al de init)
    _cascadeShadowShader.reset();
    _flatShader.reset();
    _compositeShader.reset();
    _lightShader.reset();
    _ssaoShader.reset();
    _geomShader.reset();
    _testCube.reset();
    for (auto& lod : sphereLOD) lod.reset();
    _terrainStreamingSystem.reset();
    _raycastSystem.reset();
    _planetarySystem.reset();
    _virtualTexturing.reset();
    _cascadedShadow.reset();
    _computePostProcess.reset();
    _instancing.reset();
    _lightCuller.reset();
    _bloomPong.reset();
    _bloomPing.reset();
    _bloomExtractTarget.reset();
    _lightingTarget.reset();
    _pointShadowSystem.reset();
    _worldSystem.reset();
    _iblSystem.reset();
    _ssaoSystem.reset();
    _gBuffer.reset();
    _bloomSystem.reset();
    _hdrSystem.reset();
    _shadowSystem.reset();
    _lampShader.reset();
    _mainShader.reset();
    _camera.reset();
    _currentScene.reset();

    // Quad VAO/VBO
    if (quadVAO) { glDeleteVertexArrays(1, &quadVAO); quadVAO = 0; }
    if (quadVBO) { glDeleteBuffers(1, &quadVBO);      quadVBO = 0; }

    // Solo terminar SDL3 si esta instancia creó la ventana (modo standalone).
    // En modo embebido (editor), _window es nullptr y SDL3 pertenece al editor.
    if (_glContext) {
        SDL_GL_DestroyContext(_glContext);
        _glContext = nullptr;
    }
    if (_window) {
        SDL_DestroyWindow(_window);
        _window = nullptr;
        SDL_Quit();
    }
}

void Application::run(const std::string& startScenePath) {
    create_window();
    create_gl_context();
    Haruka::Scene scene;
    if (!startScenePath.empty()) {
        scene.load(startScenePath);
    } else {
        scene = Haruka::Scene("DefaultScene");
    }
    init(scene);
    main_loop();
}