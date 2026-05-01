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
#include "world_system.h"
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
#include "project.h"
#include "error_reporter.h"
#include "renderer/gpu_instancing.h"
#include "physics/raycast_simple.h"
#include "object_types.h"
#include "core/terrain_streaming_system.h"

Haruka::GameInterface* gameInterface = nullptr;

namespace {
std::vector<const Haruka::SceneObject*> g_sceneRenderQueue;
Haruka::Scene* g_sceneForRender = nullptr;
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
    if (!obj.meshRenderer) {
        obj.meshRenderer = std::make_shared<MeshRendererComponent>();
    }
    if (!obj.meshRenderer || obj.meshRenderer->isResident()) return;
    if (!obj.properties.contains("meshRenderer")) return;

    const auto& mr = obj.properties["meshRenderer"];
    std::string meshType = mr.value("meshType", "");
    std::vector<glm::vec3> verts, norms;
    std::vector<unsigned int> indices;

    if (meshType == "cube") {
        float size = mr.value("size", 1.0f);
        PrimitiveShapes::createCube(size, verts, norms, indices);
    } else if (meshType == "sphere") {
        float radius = mr.value("radius", 1.0f);
        int segments = mr.value("segments", 32);
        PrimitiveShapes::createSphere(radius, segments, segments, verts, norms, indices);
    } else if (meshType == "capsule") {
        float radius = mr.value("radius", 0.5f);
        float height = mr.value("height", 2.0f);
        int segments = mr.value("segments", 24);
        int stacks = mr.value("stacks", 16);
        PrimitiveShapes::createCapsule(radius, height, segments, stacks, verts, norms, indices);
    } else if (meshType == "plane") {
        float width = mr.value("width", 2.0f);
        float height = mr.value("height", 2.0f);
        int subdivisions = mr.value("subdivisions", 10);
        PrimitiveShapes::createPlane(width, height, subdivisions, verts, norms, indices);
    }

    if (!verts.empty()) {
        obj.meshRenderer->setMesh(verts, norms, indices);
    }
}

void maybeReleasePrimitiveMesh(Haruka::SceneObject& obj) {
    if (obj.meshRenderer && obj.meshRenderer->isResident()) {
        obj.meshRenderer->releaseMesh();
    }
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

    // Initialize rendering systems
    _mainShader = std::make_unique<Shader>("shaders/simple.vert", "shaders/pbr.frag");
    _lampShader = std::make_unique<Shader>("shaders/simple.vert", "shaders/light_cube.frag");
    
    _shadowSystem = std::make_unique<Shadow>(1024, 1024);
    _hdrSystem = std::make_unique<HDR>(_width, _height);
    _bloomSystem = std::make_unique<Bloom>(_width, _height);
    _gBuffer = std::make_unique<GBuffer>(_width, _height);
    _ssaoSystem = std::make_unique<SSAO>(_width, _height);
    _iblSystem = std::make_unique<IBL>();
    _worldSystem = std::make_unique<Haruka::WorldSystem>();
    _pointShadowSystem = std::make_unique<PointShadow>(1024);

    _lightingTarget = std::make_unique<RenderTarget>(_width, _height);
    _bloomExtractTarget = std::make_unique<RenderTarget>(_width, _height);

    // Registrar RenderTarget por defecto (standalone), sin pisar uno externo (viewport)
    if (!MotorInstance::getInstance().getRenderTarget()) {
        MotorInstance::getInstance().setRenderTarget(_lightingTarget.get());
    }

    // Registrar Application para que scripts accedan a sistemas (raycast, etc)
    MotorInstance::getInstance().setApplication(this);

    // Inicializar light culler para soportar ilimitadas luces
    _lightCuller = std::make_unique<LightCuller>();

    // Inicializar GPU instancing para renderizado eficiente
    _instancing = std::make_unique<GPUInstancing>();
    _instancing->init(100000);  // Máximo 10k instancias por batch

    // Inicializar Asset Streaming para cargar assets bajo demanda
    AssetStreamer::getInstance().init(512, 2);  // 512 MB cache, 2 worker threads

    // Inicializar Debug Overlay para profiling
    DebugOverlay::getInstance().init();

    // Inicializar Compute Post-Processing
    _computePostProcess = std::make_unique<ComputePostProcess>();
    _computePostProcess->init(_width, _height);

    // Inicializar Cascaded Shadow Maps
    _cascadedShadow = std::make_unique<CascadedShadowMap>();
    _cascadedShadow->init(0.1f, 300000000.0f, 0.75f);  // zNear, zFar, lambda

    // Inicializar Virtual Texturing
    _virtualTexturing = std::make_unique<VirtualTexturing>();
    VTConfig vtConfig;
    vtConfig.pageSize = 128;
    vtConfig.maxResidentPages = 1024;
    vtConfig.maxCacheMemory = 256LL * 1024 * 1024;
    _virtualTexturing->init(vtConfig);

    // Inicializar Raycast System
    _planetarySystem = std::make_unique<Haruka::PlanetarySystem>();
    _raycastSystem = std::make_unique<RaycastSimple>();
    _terrainStreamingSystem = std::make_unique<Haruka::TerrainStreamingSystem>();

    setupQuad();

    // Create LOD primitives for celestial bodies
    int lodConfigs[4][2] = {
        {64, 32},  // LOD 0: High quality
        {32, 16},  // LOD 1: Medium
        {16, 8},   // LOD 2: Low
        {8, 4}     // LOD 3: Very low
    };

    for (int i = 0; i < 4; i++) {
        std::vector<glm::vec3> verts, norms;
        std::vector<unsigned int> indices;
        PrimitiveShapes::createSphere(1.0f, lodConfigs[i][0], lodConfigs[i][1], verts, norms, indices);
        sphereLOD[i] = std::make_unique<SimpleMesh>(verts, norms, indices);
    }
    
    _worldSystem->initComputeShaders();
    
    // Initialize cached shaders (avoid recreating each frame)
    _geomShader = std::make_unique<Shader>("shaders/deferred_geom.vert", "shaders/deferred_geom.frag");
    _ssaoShader = std::make_unique<Shader>("shaders/screenquad.vert", "shaders/ssao.frag");
    _lightShader = std::make_unique<Shader>("shaders/screenquad.vert", "shaders/deferred_light.frag");
    _compositeShader = std::make_unique<Shader>("shaders/screenquad.vert", "shaders/bloom_composite.frag");
    _flatShader = std::make_unique<Shader>("shaders/simple.vert", "shaders/light_cube.frag");
    _cascadeShadowShader = std::make_unique<Shader>("shaders/shadow.vert", "shaders/shadow.frag");
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

    const double nearPlane = 0.0001;
    const double farPlane = 300000000.0;
    const double aspect = (_height > 0) ? static_cast<double>(_width) / static_cast<double>(_height) : (16.0 / 9.0);

    glm::dvec3 camPos(0.0);
    glm::mat4 viewProj(1.0f);
    bool canCullByFrustum = false;

    if (activeCamera) {
        camPos = activeCamera->position;
        glm::mat4 proj = glm::perspective(glm::radians(activeCamera->zoom), static_cast<float>(aspect), static_cast<float>(nearPlane), static_cast<float>(farPlane));
        glm::mat4 view = activeCamera->getViewMatrix();
        viewProj = proj * view;
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

    s_lastVisibleChunks = terrainStats.visibleChunks;
    s_lastResidentChunks = terrainStats.residentChunks;
    s_lastPendingChunkLoads = terrainStats.pendingChunkLoads;
    s_lastPendingChunkEvictions = terrainStats.pendingChunkEvictions;
    s_lastResidentMemoryMB = terrainStats.residentMemoryMB;
    s_lastTrackedChunks = terrainStats.trackedChunks;
    s_lastMaxMemoryMB = terrainStats.maxMemoryMB;

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
        const bool isHugeBody = (maxScale > 1000.0) || (obj.name == "Earth") || (obj.name == "Sun");

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

        // Cuerpos gigantes siempre se renderizan sin culling
        if (isHugeBody) {
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
                    worldPos,
                    radius,
                    viewProj)) {
                continue;
            }
        }

        g_sceneRenderQueue.push_back(&obj);
    }

    // Contadores de total vs renderizado real
    s_lastTotalVertices = 0;
    s_lastTotalTriangles = 0;
    s_lastTotalDrawCalls = 0;
    for (const auto& obj : scene->getObjects()) {
        if (isRenderDisabledByEditor(obj)) continue;
        Haruka::ObjectType objType = Haruka::stringToObjectType(obj.type);
        if (!Haruka::isRenderableObjectType(objType)) continue;
        if (obj.meshRenderer) {
            s_lastTotalDrawCalls++;
            s_lastTotalVertices += obj.meshRenderer->getVertexCount();
            s_lastTotalTriangles += obj.meshRenderer->getTriangleCount();
        } else if (!obj.modelPath.empty()) {
            try {
                auto model = getOrLoadModel(obj.modelPath);
                if (!model) continue;
                s_lastTotalDrawCalls++;
                s_lastTotalVertices += model->getVertexCount();
                s_lastTotalTriangles += model->getTriangleCount();
            } catch (...) {}
        }
    }

    s_lastRenderedVertices = 0;
    s_lastRenderedTriangles = 0;
    s_lastRenderedDrawCalls = 0;
    for (const auto* obj : g_sceneRenderQueue) {
        if (!obj) continue;
        if (obj->meshRenderer && obj->meshRenderer->isResident()) {
            s_lastRenderedDrawCalls++;
            s_lastRenderedVertices += obj->meshRenderer->getResidentVertexCount();
            s_lastRenderedTriangles += obj->meshRenderer->getResidentTriangleCount();
        } else if (!obj->modelPath.empty()) {
            try {
                auto model = getOrLoadModel(obj->modelPath);
                if (!model) continue;
                s_lastRenderedDrawCalls++;
                s_lastRenderedVertices += model->getVertexCount();
                s_lastRenderedTriangles += model->getTriangleCount();
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
    Haruka::Scene* scene = MotorInstance::getInstance().getScene();
    if (!scene && !_currentScene) return;
    if (!_camera) return;

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
    // ========== SETUP ==========
    Camera* activeCamera = MotorInstance::getInstance().getCamera();
    if (!activeCamera) {
        activeCamera = _camera.get();
    }
    if (!activeCamera) return;

    glm::mat4 proj = glm::perspective(glm::radians(activeCamera->zoom), (float)_width / (float)_height, 0.0001f, 300000000.0f);
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
    RenderTarget* editorTarget = MotorInstance::getInstance().getRenderTarget();
    if (editorTarget) {
        buildRenderQueue();

        editorTarget->bindForWriting();
        glEnable(GL_DEPTH_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        _flatShader->use();
        _flatShader->setMat4("projection", proj);
        _flatShader->setMat4("view", view);

        glm::vec3 sunDir = glm::normalize(glm::vec3(0.3f, 0.6f, 0.7f));
        glm::vec3 sunLightColor = glm::vec3(1.0f);
        for (const auto* obj : g_sceneRenderQueue) {
            if (!obj) continue;
            if (obj->type == "Light" || obj->type == "PointLight" || obj->type == "DirectionalLight") {
                glm::vec3 p = glm::vec3(obj->getWorldPosition(g_sceneForRender));
                if (glm::length(p) > 0.0001f) {
                    sunDir = glm::normalize(p);
                }
                float sunEnergy = std::clamp(std::max((float)obj->intensity, 0.0f) * 0.01f, 0.2f, 2.0f);
                sunLightColor = glm::vec3(obj->color) * sunEnergy;
                break;
            }
        }
        _flatShader->setVec3("sunDirection", sunDir);
        _flatShader->setVec3("sunLightColor", sunLightColor);
        _flatShader->setFloat("ambientStrength", 0.12f);

        for (const auto* obj : g_sceneRenderQueue) {
            if (!obj) continue;
            glm::mat4 modelMatrix = g_sceneForRender ? obj->getWorldTransform(g_sceneForRender) : glm::mat4(1.0f);
            _flatShader->setMat4("model", modelMatrix);
            glm::vec3 baseColor = glm::vec3(obj->color);
            if (glm::length(baseColor) < 0.001f) baseColor = glm::vec3(0.8f, 0.8f, 0.8f);
            const bool isLightObj = (obj->type == "Light" || obj->type == "PointLight" || obj->type == "DirectionalLight");
            float emission = isLightObj ? std::max((float)obj->intensity, 0.0f) : 1.0f;
            glm::vec3 c = isLightObj ? (baseColor * emission) : baseColor;
            _flatShader->setVec3("lightColor", c);
            _flatShader->setBool("useProceduralTerrain", !isLightObj && shouldUseProceduralTerrainLook(*obj));
            if (obj->meshRenderer && obj->meshRenderer->isResident()) {
                obj->meshRenderer->render(*_flatShader);
                continue;
            }
            if (!obj->modelPath.empty()) {
                try {
                    auto model = getOrLoadModel(obj->modelPath);
                    if (model) model->Draw(*_flatShader);
                } catch (...) {
                    // Ignorar en fallback
                }
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
        0.0001f,
        300000000.0f,
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
        if (obj->material) {
            _geomShader->setVec3("material.albedo", obj->material->albedo);
            _geomShader->setFloat("material.roughness", obj->material->roughness);
            _geomShader->setFloat("material.metallic", obj->material->metallic);
        }
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
    // Render celestial bodies with LOD
    for (const auto& body : _worldSystem->getBodies()) {
        if (!body.visible) continue;
        if (g_sceneForRender && g_sceneForRender->getObject(body.name)) continue;

        glm::vec3 camPos(activeCamera->position.x, activeCamera->position.y, activeCamera->position.z);
        glm::vec3 bodyPos(body.localPos.x, body.localPos.y, body.localPos.z);
        float distance = glm::length(camPos - bodyPos);
        
        int lod = distance < 50.0f ? 0 : distance < 200.0f ? 1 : distance < 1000.0f ? 2 : 3;
        
        if (!sphereLOD[lod]) continue;

        glm::mat4 bodyModel = glm::translate(glm::mat4(1.0f), glm::vec3(body.localPos));
        bodyModel = glm::scale(bodyModel, glm::vec3(body.radius));
        
        _geomShader->setMat4("model", bodyModel);
        sphereLOD[lod]->draw();
    }
        
    _gBuffer->unbind();

    // ========== SSAO PASS ==========
    _ssaoSystem->bindForWriting();
    glClear(GL_COLOR_BUFFER_BIT);

    _ssaoShader->use();
    _gBuffer->bindForReading(0, 0);
    _gBuffer->bindForReading(1, 1);
    
    _ssaoShader->setInt("gPosition", 0);
    _ssaoShader->setInt("gNormal", 1);
    _ssaoShader->setInt("texNoise", 2);
    _ssaoShader->setInt("kernelSize", 64);
    _ssaoShader->setFloat("radius", 0.5f);
    _ssaoShader->setFloat("bias", 0.025f);
    _ssaoShader->setMat4("projection", proj);

    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    _ssaoSystem->unbind();

    // ========== LIGHTING PASS ==========
    _lightingTarget->bindForWriting();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    _lightShader->use();

    _gBuffer->bindForReading(0, 0);
    _gBuffer->bindForReading(1, 1);
    _gBuffer->bindForReading(2, 2);
    _gBuffer->bindForReading(3, 3);
    _ssaoSystem->bindForReading(4);
    _iblSystem->bindPrefilterMap(5);
    _iblSystem->bindBRDFLUT(6);

    _lightShader->setInt("gPosition", 0);
    _lightShader->setInt("gNormal", 1);
    _lightShader->setInt("gAlbedoSpec", 2);
    _lightShader->setInt("gEmissive", 3);
    _lightShader->setInt("ssao", 4);
    _lightShader->setInt("prefilterMap", 5);
    _lightShader->setInt("brdfLUT", 6);
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
    
    _lightShader->setInt("numLights", culledLights.size());
    for (size_t i = 0; i < culledLights.size(); i++) {
        _lightShader->setVec3("lights[" + std::to_string(i) + "].position", culledLights[i].position);
        _lightShader->setVec3("lights[" + std::to_string(i) + "].color", culledLights[i].color);
    }

    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    _lightingTarget->unbind();

    // ========== FINAL COMPOSITE ==========
    RenderTarget* externalTarget = MotorInstance::getInstance().getRenderTarget();
    if (externalTarget) {
        externalTarget->bindForWriting();
    }
    glViewport(0, 0, _width, _height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    _compositeShader->use();
    _lightingTarget->bindForReading(0);
    _bloomExtractTarget->bindForReading(1);
    _compositeShader->setInt("scene", 0);
    _compositeShader->setInt("bloom", 1);
    _compositeShader->setFloat("bloomStrength", 0.0f);

    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    if (externalTarget) {
        externalTarget->unbind();
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
    for (auto& lod : sphereLOD) lod.reset();
    _terrainStreamingSystem.reset();
    _raycastSystem.reset();
    _planetarySystem.reset();
    _virtualTexturing.reset();
    _cascadedShadow.reset();
    _computePostProcess.reset();
    _instancing.reset();
    _lightCuller.reset();
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