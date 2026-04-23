#include "application.h"

#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdexcept>
#include <vector>
#include <iostream>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <chrono>
#include <cassert>

#include "error_reporter.h"
#include "core/game_interface.h"
#include "core/object_types.h"
#include "core/terrain_streaming_system.h"
#include "renderer/mesh.h"
#include "renderer/motor_instance.h"
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
#include "renderer/gpu_instancing.h"
#include "physics/raycast_simple.h"

Haruka::GameInterface* gameInterface = nullptr;

// Mouse input state
struct MouseState {
    float lastX = 640.0f;
    float lastY = 360.0f;
    bool firstMouse = true;
};

static MouseState g_mouseState;

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

bool isTerrainChunkFacingCamera(const Haruka::SceneObject& obj, const glm::vec3& cameraPos) {
    if (!obj.properties.is_object()) return true;
    if (!obj.properties.contains("terrainEditor")) return true;
    const auto& te = obj.properties["terrainEditor"];
    if (!te.is_object() || !te.value("isChunk", false)) return true;

    // En runtime exigimos metadatos explícitos de chunk.
    if (!te.contains("chunkX") || !te.contains("chunkY") || !te.contains("chunkTilesX") || !te.contains("chunkTilesY")) {
        return true;
    }

    const int chunkX = te.value("chunkX", -1);
    const int chunkY = te.value("chunkY", -1);
    const int tilesX = te.value("chunkTilesX", 0);
    const int tilesY = te.value("chunkTilesY", 0);
    if (chunkX < 0 || chunkY < 0 || tilesX <= 0 || tilesY <= 0) return true;

    const double pi = 3.14159265358979323846;
    const float lat = ((static_cast<float>(chunkY) + 0.5f) / static_cast<float>(tilesY)) * static_cast<float>(pi) - static_cast<float>(pi * 0.5);
    const float lon = ((static_cast<float>(chunkX) + 0.5f) / static_cast<float>(tilesX)) * static_cast<float>(2.0 * pi) - static_cast<float>(pi);

    glm::vec3 chunkDir(
        std::cos(lat) * std::cos(lon),
        std::sin(lat),
        std::cos(lat) * std::sin(lon)
    );

    glm::vec3 camDir = cameraPos;
    float camLen = glm::length(camDir);
    if (camLen > 1e-6f) camDir /= camLen;
    else camDir = glm::vec3(0.0f, 0.0f, 1.0f);

    return glm::dot(chunkDir, camDir) > -0.15f;
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

Application::Application() : _window(nullptr) {
    vkShadowTarget = std::make_unique<RenderTarget>(_width, _height);
    vkBloomTarget = std::make_unique<RenderTarget>(_width, _height);
    vkPostprocessTarget = std::make_unique<RenderTarget>(_width, _height);
    _shadowSystem = std::make_unique<Shadow>(2048, 2048);
}

Application::~Application() {
    if (vkDevice != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(vkDevice);
    }
    // Destruir render targets Vulkan
    if (vkShadowTarget) { vkShadowTarget->destroyVulkanResources(); vkShadowTarget.reset(); }
    if (vkBloomTarget) { vkBloomTarget->destroyVulkanResources(); vkBloomTarget.reset(); }
    if (vkPostprocessTarget) { vkPostprocessTarget->destroyVulkanResources(); vkPostprocessTarget.reset(); }
    if (_shadowSystem) { _shadowSystem->destroyResources(); _shadowSystem.reset(); }
    // Destruir semáforos y fences
    if (vkShadowToGeometrySemaphore) { vkDestroySemaphore(vkDevice, vkShadowToGeometrySemaphore, nullptr); vkShadowToGeometrySemaphore = VK_NULL_HANDLE; }
    if (vkGeometryToLightingSemaphore) { vkDestroySemaphore(vkDevice, vkGeometryToLightingSemaphore, nullptr); vkGeometryToLightingSemaphore = VK_NULL_HANDLE; }
    if (vkLightingToPostprocessSemaphore) { vkDestroySemaphore(vkDevice, vkLightingToPostprocessSemaphore, nullptr); vkLightingToPostprocessSemaphore = VK_NULL_HANDLE; }
    if (vkImageAvailableSemaphore) { vkDestroySemaphore(vkDevice, vkImageAvailableSemaphore, nullptr); vkImageAvailableSemaphore = VK_NULL_HANDLE; }
    if (vkRenderFinishedSemaphore) { vkDestroySemaphore(vkDevice, vkRenderFinishedSemaphore, nullptr); vkRenderFinishedSemaphore = VK_NULL_HANDLE; }
    if (vkInFlightFence) { vkDestroyFence(vkDevice, vkInFlightFence, nullptr); vkInFlightFence = VK_NULL_HANDLE; }
    // Destruir framebuffers
    for (auto fb : vkFramebuffers) {
        if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(vkDevice, fb, nullptr);
    }
    vkFramebuffers.clear();
    // Destruir render pass
    if (vkRenderPass) { vkDestroyRenderPass(vkDevice, vkRenderPass, nullptr); vkRenderPass = VK_NULL_HANDLE; }
    // Destruir swapchain image views
    for (auto view : vkSwapchainImageViews) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(vkDevice, view, nullptr);
    }
    vkSwapchainImageViews.clear();
    // Destruir swapchain
    if (vkSwapchain) { vkDestroySwapchainKHR(vkDevice, vkSwapchain, nullptr); vkSwapchain = VK_NULL_HANDLE; }
    // Destruir command pool
    if (vkCommandPool) { vkDestroyCommandPool(vkDevice, vkCommandPool, nullptr); vkCommandPool = VK_NULL_HANDLE; }
    // Destruir descriptor pool y layouts
    if (vkDescriptorPool) { vkDestroyDescriptorPool(vkDevice, vkDescriptorPool, nullptr); vkDescriptorPool = VK_NULL_HANDLE; }
    if (vkMaterialDescriptorSetLayout) { vkDestroyDescriptorSetLayout(vkDevice, vkMaterialDescriptorSetLayout, nullptr); vkMaterialDescriptorSetLayout = VK_NULL_HANDLE; }
    if (vkUniformDescriptorSetLayout) { vkDestroyDescriptorSetLayout(vkDevice, vkUniformDescriptorSetLayout, nullptr); vkUniformDescriptorSetLayout = VK_NULL_HANDLE; }
    // Destruir uniform buffer
    if (vkUniformBuffer) { vkDestroyBuffer(vkDevice, vkUniformBuffer, nullptr); vkUniformBuffer = VK_NULL_HANDLE; }
    if (vkUniformBufferMemory) { vkFreeMemory(vkDevice, vkUniformBufferMemory, nullptr); vkUniformBufferMemory = VK_NULL_HANDLE; }
    // Destruir device
    if (vkDevice) { vkDestroyDevice(vkDevice, nullptr); vkDevice = VK_NULL_HANDLE; }
    // Destruir surface
    if (vkSurface) { vkDestroySurfaceKHR(vkInstance, vkSurface, nullptr); vkSurface = VK_NULL_HANDLE; }
    // Destruir instance
    if (vkInstance) { vkDestroyInstance(vkInstance, nullptr); vkInstance = VK_NULL_HANDLE; }

    // Verificación de recursos Vulkan
    assert(vkFramebuffers.empty() && "vkFramebuffers no está vacío tras destrucción");
    assert(vkSwapchainImageViews.empty() && "vkSwapchainImageViews no está vacío tras destrucción");
    assert(vkSwapchain == VK_NULL_HANDLE && "vkSwapchain no destruido");
    assert(vkCommandPool == VK_NULL_HANDLE && "vkCommandPool no destruido");
    assert(vkDescriptorPool == VK_NULL_HANDLE && "vkDescriptorPool no destruido");
    assert(vkUniformBuffer == VK_NULL_HANDLE && "vkUniformBuffer no destruido");
    assert(vkUniformBufferMemory == VK_NULL_HANDLE && "vkUniformBufferMemory no destruido");
    assert(vkDevice == VK_NULL_HANDLE && "vkDevice no destruido");
    assert(vkInstance == VK_NULL_HANDLE && "vkInstance no destruido");
}

void Application::cleanup() {
    // Limpiar registro en MotorInstance
    MotorInstance::getInstance().clear();
    // Limpiar recursos de sistemas
    _mainShader.reset();
    _lampShader.reset();
    _shadowSystem.reset();
    _hdrSystem.reset();
    _bloomSystem.reset();
    _gBuffer.reset();
    _ssaoSystem.reset();
    _iblSystem.reset();
    _worldSystem.reset();
    _pointShadowSystem.reset();
    _lightingTarget.reset();
    _bloomExtractTarget.reset();
    _lightCuller.reset();
    _instancing.reset();
    _computePostProcess.reset();
    _cascadedShadow.reset();
    _virtualTexturing.reset();
    _planetarySystem.reset();
    _raycastSystem.reset();
    _terrainStreamingSystem.reset();
    _camera.reset();
    _currentScene.reset();
    for (auto& lod : sphereLOD) lod.reset();
}

void Application::run(const std::string& startScenePath) {
    create_window();
    Haruka::Scene scene;
    if (!startScenePath.empty()) {
        scene.load(startScenePath);
    } else {
        scene = Haruka::Scene("DefaultScene");
    }
    init(scene);
    main_loop();
}

void Application::create_window() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        HARUKA_MOTOR_ERROR(ErrorCode::MOTOR_INIT_FAILED, "Failed to initialize SDL");
        throw std::runtime_error("Fallo al inicializar SDL");
    }

    // Crear ventana con soporte Vulkan
    _window = SDL_CreateWindow(
        "Haruka Engine",
        _width,
        _height,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE
    );
    if (!_window) {
        HARUKA_MOTOR_ERROR(ErrorCode::WINDOW_CREATION_FAILED, "Failed to create SDL window");
        throw std::runtime_error("Fallo al crear la ventana");
    }

    // El manejo de eventos se realiza en el main loop (ver main_loop())
    // Para cerrar con Escape o la X, ver ejemplo en main_loop:
    // while (SDL_PollEvent(&event)) {
    //     if (event.type == SDL_EVENT_QUIT) running = false;
    //     if (event.type == SDL_EVENT_KEY_DOWN && event.key.keysym.sym == SDLK_ESCAPE) running = false;
    // }
}

void Application::init(Haruka::Scene& scene) {
    
    _currentScene = std::make_unique<Haruka::Scene>(scene);
    // Usar siempre la cámara de la interfaz de juego si está disponible
    Camera* sceneCamera = nullptr;
    if (gameInterface && gameInterface->getCamera) {
        sceneCamera = gameInterface->getCamera();
        std::cout << (sceneCamera ? "✓ Camera from game interface" : "⚠ Game interface did not provide a camera") << std::endl;
    }

    if (sceneCamera) {
        _camera = std::make_unique<Camera>(sceneCamera->position);
        _camera->orientation = sceneCamera->orientation;
        _camera->zoom = sceneCamera->zoom;
        _camera->speed = sceneCamera->speed;
        _camera->sensitivity = sceneCamera->sensitivity;
    } else {
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
    _pointShadowSystem = std::make_unique<PointShadow>(1024, 1024);

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

    // Inicializar Asset Streaming para cargar assets bajo demanda
    AssetStreamer::getInstance().init(512, 2);  // 512 MB cache, 2 worker threads

    // Inicializar Debug Overlay para profiling
    DebugOverlay::getInstance().init();

    _computePostProcess = std::make_unique<ComputePostprocess>(_width, _height);
    _cascadedShadow = std::make_unique<CascadedShadow>(_width, _height, 4);
    _virtualTexturing = std::make_unique<VirtualTexturing>(_width, _height);

    // Inicializar Raycast System
    _planetarySystem = std::make_unique<Haruka::PlanetarySystem>();
    _raycastSystem = std::make_unique<RaycastSimple>();
    _terrainStreamingSystem = std::make_unique<Haruka::TerrainStreamingSystem>();

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
        sphereLOD[i] = std::make_unique<SimpleMesh>(verts, indices);
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

void Application::renderScene(Shader* shader) {
    (void)shader; // Esta función solo construye la cola de render

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
    glm::mat4 view = activeCamera ? activeCamera->getViewMatrix() : glm::mat4(1.0f);
    glm::mat4 proj = glm::perspective(glm::radians(activeCamera ? activeCamera->zoom : 60.0f), (float)_width / (float)_height, (float)nearPlane, (float)farPlane);
    glm::mat4 viewProj = proj * view;

    for (const auto& obj : scene->getObjects()) {
        if (isRenderDisabledByEditor(obj)) continue;
        Haruka::ObjectType objType = Haruka::stringToObjectType(obj.type);
        if (!Haruka::isRenderableObjectType(objType)) continue;

        glm::dvec3 worldPos = obj.getWorldPosition(scene);
        glm::dvec3 camPos = activeCamera ? activeCamera->position : glm::dvec3(0.0);
        glm::dvec3 worldScale = obj.getWorldScale(scene);
        double radius = obj.meshRenderer ? obj.meshRenderer->getBoundingRadius() : 1.0;
        int layer = obj.renderLayer;
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
                    maybeReleasePrimitiveMesh(const_cast<Haruka::SceneObject&>(obj));
                    continue;
                }
            }
        }

        // Cuerpos gigantes siempre se renderizan sin culling
        if (isHugeBody) {
            g_sceneRenderQueue.push_back(&obj);
            continue;
        }

        // Para objetos normales, aplicar culling por frustum y distancia
        bool canCullByFrustum = true;
        if (canCullByFrustum) {
            double radiusCulling = std::max(0.5 * glm::length(worldScale), maxScale);
            if (radiusCulling < 0.001) {
                g_sceneRenderQueue.push_back(&obj);
                continue;
            }
            if (!isSphereInsideCameraFrustum(worldPos, radiusCulling, viewProj)) {
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

void Application::create_vulkan_context() {
    printf("[Haruka] >> INICIO create_vulkan_context\n");
    // === INICIALIZACIÓN VULKAN ORDENADA ===
    // 1. Instance
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "HarukaEngine";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "HarukaEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    Uint32 sdlExtensionCount = 0;
    const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
    if (!sdlExtensions) {
        throw std::runtime_error("SDL_Vulkan_GetInstanceExtensions failed");
    }

    createInfo.enabledExtensionCount = sdlExtensionCount;
    createInfo.ppEnabledExtensionNames = sdlExtensions;
    createInfo.enabledLayerCount = 0;

    VkResult res = vkCreateInstance(&createInfo, nullptr, &vkInstance);
    printf("[Haruka] vkCreateInstance: %d\n", res);
    if (res != VK_SUCCESS) {
        throw std::runtime_error("Fallo al crear VkInstance");
    }

    // 2. Surface
    if (!SDL_Vulkan_CreateSurface(_window, vkInstance, nullptr, &vkSurface)) {
        printf("[Haruka] Error: SDL_Vulkan_CreateSurface\n");
        throw std::runtime_error("Fallo al crear VkSurfaceKHR con SDL");
    }

    // 3. Physical Device
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(vkInstance, &deviceCount, nullptr);
    printf("[Haruka] GPUs detectadas: %u\n", deviceCount);
    if (deviceCount == 0) throw std::runtime_error("No hay GPUs con soporte Vulkan");
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(vkInstance, &deviceCount, devices.data());
    vkPhysicalDevice = devices[0];

    // 4. Logical Device & Queues
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = 0; // Índice de la cola de gráficos
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    const std::vector<const char*> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };

    VkDeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;

    // Activación explícita de la extensión del Swapchain
    deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions.data();

    res = vkCreateDevice(vkPhysicalDevice, &deviceCreateInfo, nullptr, &vkDevice);
    printf("[Haruka] vkCreateDevice: %d\n", res);
    if (res != VK_SUCCESS) {
        throw std::runtime_error("Fallo al crear VkDevice");
    }
    
    vkGetDeviceQueue(vkDevice, 0, 0, &vkGraphicsQueue);
    vkGetDeviceQueue(vkDevice, 0, 0, &vkPresentQueue);
    printf("[Haruka] vkGetDeviceQueue OK\n");

    // 5. Swapchain
    VkSwapchainCreateInfoKHR swapchainCreateInfo{};
    swapchainCreateInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainCreateInfo.surface = vkSurface;
    swapchainCreateInfo.minImageCount = 2;
    swapchainCreateInfo.imageFormat = VK_FORMAT_B8G8R8A8_SRGB;
    swapchainCreateInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swapchainCreateInfo.imageExtent = { (uint32_t)_width, (uint32_t)_height };
    swapchainCreateInfo.imageArrayLayers = 1;
    swapchainCreateInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchainCreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainCreateInfo.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapchainCreateInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainCreateInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchainCreateInfo.clipped = VK_TRUE;
    swapchainCreateInfo.oldSwapchain = VK_NULL_HANDLE;
    res = vkCreateSwapchainKHR(vkDevice, &swapchainCreateInfo, nullptr, &vkSwapchain);
    printf("[Haruka] vkCreateSwapchainKHR: %d\n", res);
    if (res != VK_SUCCESS) {
        throw std::runtime_error("Fallo al crear VkSwapchainKHR");
    }
    printf("[Haruka] << FIN create_vulkan_context\n");
    printf("[Haruka] >> INICIO init\n");
    printf("[Haruka] << FIN init\n");
    printf("[Haruka] >> INICIO run\n");
    printf("[Haruka] << FIN run\n");

    // 6. Swapchain Images & Views
    uint32_t imageCount = 0;
    vkGetSwapchainImagesKHR(vkDevice, vkSwapchain, &imageCount, nullptr);
    vkSwapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(vkDevice, vkSwapchain, &imageCount, vkSwapchainImages.data());
    vkSwapchainImageViews.resize(imageCount);
    for (size_t i = 0; i < imageCount; ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = vkSwapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_B8G8R8A8_SRGB;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(vkDevice, &viewInfo, nullptr, &vkSwapchainImageViews[i]) != VK_SUCCESS) {
            throw std::runtime_error("Fallo al crear VkImageView de swapchain");
        }
    }

    // 7. Sync Primitives
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(vkDevice, &semInfo, nullptr, &vkImageAvailableSemaphore);
    vkCreateSemaphore(vkDevice, &semInfo, nullptr, &vkRenderFinishedSemaphore);
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(vkDevice, &fenceInfo, nullptr, &vkInFlightFence);

    // 8. Render Pass
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = VK_FORMAT_B8G8R8A8_SRGB;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    if (vkCreateRenderPass(vkDevice, &renderPassInfo, nullptr, &vkRenderPass) != VK_SUCCESS) {
        throw std::runtime_error("Fallo al crear VkRenderPass");
    }

    // 9. Framebuffers
    vkFramebuffers.resize(vkSwapchainImageViews.size());
    for (size_t i = 0; i < vkSwapchainImageViews.size(); ++i) {
        VkImageView attachments[] = { vkSwapchainImageViews[i] };
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = vkRenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = _width;
        framebufferInfo.height = _height;
        framebufferInfo.layers = 1;
        if (vkCreateFramebuffer(vkDevice, &framebufferInfo, nullptr, &vkFramebuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("Fallo al crear VkFramebuffer");
        }
    }

    // 10. Command Pool
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = 0;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &vkCommandPool) != VK_SUCCESS) {
        throw std::runtime_error("Fallo al crear VkCommandPool");
    }

    // 11. Command Buffers
    vkCommandBuffers.resize(vkFramebuffers.size());
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = vkCommandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(vkCommandBuffers.size());
    if (vkAllocateCommandBuffers(vkDevice, &allocInfo, vkCommandBuffers.data()) != VK_SUCCESS) {
        throw std::runtime_error("Fallo al crear VkCommandBuffer");
    }

    // 12. PIPELINE Y LAYOUT (estructura, no dummy)
    // TODO: Crear VkDescriptorSetLayout y VkPipelineLayout para cada pass
    // Ejemplo para geometry pass:
    // VkDescriptorSetLayoutBinding bindings[] = { ... };
    // VkDescriptorSetLayoutCreateInfo layoutInfo{};
    // layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    // layoutInfo.bindingCount = ...;
    // layoutInfo.pBindings = bindings;
    // vkCreateDescriptorSetLayout(...);
    // VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    // pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    // pipelineLayoutInfo.setLayoutCount = ...;
    // pipelineLayoutInfo.pSetLayouts = ...;
    // vkCreatePipelineLayout(vkDevice, &pipelineLayoutInfo, nullptr, &vkGeometryPipelineLayout);
    // TODO: Crear VkPipeline para cada pass (geometry, shadow, etc.) usando los estados y shaders reales
    // VkGraphicsPipelineCreateInfo pipelineInfo{};
    // pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    // pipelineInfo.layout = vkGeometryPipelineLayout;
    // pipelineInfo.renderPass = vkRenderPass;
    // ...
    // vkCreateGraphicsPipelines(..., &vkGeometryPipeline);

    // --- DESCRIPTOR SET LAYOUT Y POOL PARA MATERIALES ---
    VkDescriptorSetLayoutBinding samplerLayoutBinding{};
    samplerLayoutBinding.binding = 0;
    samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerLayoutBinding.descriptorCount = 1;
    samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    samplerLayoutBinding.pImmutableSamplers = nullptr;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &samplerLayoutBinding;
    vkCreateDescriptorSetLayout(vkDevice, &layoutInfo, nullptr, &vkMaterialDescriptorSetLayout);

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 128; // Máximo de materiales
    VkDescriptorPoolCreateInfo poolInfoDS{};
    poolInfoDS.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfoDS.poolSizeCount = 1;
    poolInfoDS.pPoolSizes = &poolSize;
    poolInfoDS.maxSets = 128;
    vkCreateDescriptorPool(vkDevice, &poolInfoDS, nullptr, &vkDescriptorPool);

    // --- CREAR DESCRIPTOR SETS PARA MATERIALES (ejemplo: uno por mesh/texture) ---
    // vkMaterialDescriptorSets.resize(sceneMeshes.size());
    // std::vector<VkDescriptorSetLayout> layouts(sceneMeshes.size(), vkMaterialDescriptorSetLayout);
    // VkDescriptorSetAllocateInfo allocInfoDS{};
    // allocInfoDS.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    // allocInfoDS.descriptorPool = vkDescriptorPool;
    // allocInfoDS.descriptorSetCount = (uint32_t)sceneMeshes.size();
    // allocInfoDS.pSetLayouts = layouts.data();
    // vkAllocateDescriptorSets(vkDevice, &allocInfoDS, vkMaterialDescriptorSets.data());
    // for (size_t i = 0; i < sceneMeshes.size(); ++i) {
    //     VkDescriptorImageInfo imageInfo{};
    //     imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    //     imageInfo.imageView = sceneMeshes[i]->textures[0].vkImageView;
    //     imageInfo.sampler = sceneMeshes[i]->textures[0].vkSampler;
    //     VkWriteDescriptorSet descriptorWrite{};
    //     descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    //     descriptorWrite.dstSet = vkMaterialDescriptorSets[i];
    //     descriptorWrite.dstBinding = 0;
    //     descriptorWrite.dstArrayElement = 0;
    //     descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    //     descriptorWrite.descriptorCount = 1;
    //     descriptorWrite.pImageInfo = &imageInfo;
    //     vkUpdateDescriptorSets(vkDevice, 1, &descriptorWrite, 0, nullptr);
    // }

    // --- UNIFORM BUFFER Y DESCRIPTOR SET LAYOUT PARA MATRICES (MVP) ---
    VkDeviceSize bufferSize = sizeof(glm::mat4) * 3; // model, view, proj
    VkBufferCreateInfo uboInfo{};
    uboInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    uboInfo.size = bufferSize;
    uboInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    uboInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(vkDevice, &uboInfo, nullptr, &vkUniformBuffer);
    VkMemoryRequirements uboMemReq;
    vkGetBufferMemoryRequirements(vkDevice, vkUniformBuffer, &uboMemReq);
    VkMemoryAllocateInfo uboAllocInfo{};
    uboAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    uboAllocInfo.allocationSize = uboMemReq.size;
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(vkPhysicalDevice, &memProperties);
    uint32_t memoryTypeIndex = 0;
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((uboMemReq.memoryTypeBits & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memoryTypeIndex = i;
            break;
        }
    }
    uboAllocInfo.memoryTypeIndex = memoryTypeIndex;
    vkAllocateMemory(vkDevice, &uboAllocInfo, nullptr, &vkUniformBufferMemory);
    vkBindBufferMemory(vkDevice, vkUniformBuffer, vkUniformBufferMemory, 0);

    VkDescriptorSetLayoutBinding uboLayoutBinding{};
    uboLayoutBinding.binding = 0;
    uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboLayoutBinding.descriptorCount = 1;
    uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    uboLayoutBinding.pImmutableSamplers = nullptr;
    VkDescriptorSetLayoutCreateInfo uboLayoutInfo{};
    uboLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    uboLayoutInfo.bindingCount = 1;
    uboLayoutInfo.pBindings = &uboLayoutBinding;
    vkCreateDescriptorSetLayout(vkDevice, &uboLayoutInfo, nullptr, &vkUniformDescriptorSetLayout);

    // --- Asignar descriptor set para uniform buffer ---
    VkDescriptorSetAllocateInfo uboAllocInfoDS{};
    uboAllocInfoDS.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    uboAllocInfoDS.descriptorPool = vkDescriptorPool;
    uboAllocInfoDS.descriptorSetCount = 1;
    uboAllocInfoDS.pSetLayouts = &vkUniformDescriptorSetLayout;
    vkAllocateDescriptorSets(vkDevice, &uboAllocInfoDS, &vkUniformDescriptorSet);
    VkDescriptorBufferInfo bufferInfoDS{};
    bufferInfoDS.buffer = vkUniformBuffer;
    bufferInfoDS.offset = 0;
    bufferInfoDS.range = bufferSize;
    VkWriteDescriptorSet uboWrite{};
    uboWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    uboWrite.dstSet = vkUniformDescriptorSet;
    uboWrite.dstBinding = 0;
    uboWrite.dstArrayElement = 0;
    uboWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboWrite.descriptorCount = 1;
    uboWrite.pBufferInfo = &bufferInfoDS;
    vkUpdateDescriptorSets(vkDevice, 1, &uboWrite, 0, nullptr);

    for (size_t i = 0; i < vkCommandBuffers.size(); ++i) {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(vkCommandBuffers[i], &beginInfo);

        // --- SHADOW PASS ---
        if (_shadowSystem) {
            _shadowSystem->bindForWriting(vkCommandBuffers[i]);
            VkViewport viewport{};
            viewport.x = 0.0f;
            viewport.y = 0.0f;
            viewport.width = static_cast<float>(_shadowSystem->shadowWidth);
            viewport.height = static_cast<float>(_shadowSystem->shadowHeight);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(vkCommandBuffers[i], 0, 1, &viewport);
            VkRect2D scissor{};
            scissor.offset = {0, 0};
            scissor.extent = { _shadowSystem->shadowWidth, _shadowSystem->shadowHeight };
            vkCmdSetScissor(vkCommandBuffers[i], 0, 1, &scissor);
            // Aquí deberías hacer bind de pipeline, descriptor sets y draw calls para las sombras
            // vkCmdBindPipeline(...)
            // vkCmdBindDescriptorSets(...)
            // vkCmdDraw/Indexed(...)
            _shadowSystem->unbind(vkCommandBuffers[i]);
        }

    }
}

// Vulkan main loop: acquire, submit, present
void Application::main_loop() {
        printf("[Haruka] >> INICIO main_loop\n");
        printf("[Haruka] << FIN main_loop\n");
    bool running = true;
    SDL_Event event;
    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
            // Aquí puedes manejar otros eventos SDL si lo necesitas
        }
        renderFrame();
    }
    vkDeviceWaitIdle(vkDevice);
}

void Application::renderFrame() {
        printf("[Haruka] >> INICIO renderFrame\n");
        printf("[Haruka] << FIN renderFrame\n");
    auto frameStart = std::chrono::high_resolution_clock::now();
    vkWaitForFences(vkDevice, 1, &vkInFlightFence, VK_TRUE, UINT64_MAX);
    vkResetFences(vkDevice, 1, &vkInFlightFence);

    uint32_t imageIndex = 0;
    VkResult acquireResult = vkAcquireNextImageKHR(
        vkDevice, vkSwapchain, UINT64_MAX, vkImageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR || acquireResult == VK_SUBOPTIMAL_KHR) {
        recreateSwapchainAndResources();
        return;
    } else if (acquireResult == VK_ERROR_DEVICE_LOST) {
        fprintf(stderr, "[Vulkan] VK_ERROR_DEVICE_LOST en acquire. Intentando reinicializar contexto...\n");
        recreateSwapchainAndResources();
        return;
    } else if (acquireResult != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan] Error inesperado en vkAcquireNextImageKHR: %d\n", acquireResult);
        recreateSwapchainAndResources();
        return;
    }

    vkResetCommandBuffer(vkCommandBuffers[imageIndex], 0);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(vkCommandBuffers[imageIndex], &beginInfo);
    // Escribir timestamp de inicio
    if (g_gpuQueryPool != VK_NULL_HANDLE)
        vkCmdResetQueryPool(vkCommandBuffers[imageIndex], g_gpuQueryPool, 0, 2);
    if (g_gpuQueryPool != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(vkCommandBuffers[imageIndex], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_gpuQueryPool, 0);
    renderFrameContentVulkan(vkCommandBuffers[imageIndex], imageIndex);
    // Escribir timestamp de fin
    if (g_gpuQueryPool != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(vkCommandBuffers[imageIndex], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_gpuQueryPool, 1);
    vkEndCommandBuffer(vkCommandBuffers[imageIndex]);

    VkPipelineStageFlags waitStagesGeom[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSubmitInfo submitGeom{};
    submitGeom.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitGeom.waitSemaphoreCount = 1;
    submitGeom.pWaitSemaphores = &vkImageAvailableSemaphore;
    submitGeom.pWaitDstStageMask = waitStagesGeom;
    submitGeom.commandBufferCount = 1;
    submitGeom.pCommandBuffers = &vkCommandBuffers[imageIndex];
    submitGeom.signalSemaphoreCount = 1;
    submitGeom.pSignalSemaphores = &vkGeometryToLightingSemaphore;
    if (vkQueueSubmit(vkGraphicsQueue, 1, &submitGeom, VK_NULL_HANDLE) != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan] Error en vkQueueSubmit (geometry pass). Intentando recrear recursos...\n");
        recreateSwapchainAndResources();
        return;
    }
    VkPipelineStageFlags waitStagesLight[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSubmitInfo submitLight{};
    submitLight.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitLight.waitSemaphoreCount = 1;
    submitLight.pWaitSemaphores = &vkGeometryToLightingSemaphore;
    submitLight.pWaitDstStageMask = waitStagesLight;
    submitLight.commandBufferCount = 1;
    submitLight.pCommandBuffers = &vkCommandBuffers[imageIndex];
    submitLight.signalSemaphoreCount = 1;
    submitLight.pSignalSemaphores = &vkLightingToPostprocessSemaphore;
    if (vkQueueSubmit(vkGraphicsQueue, 1, &submitLight, VK_NULL_HANDLE) != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan] Error en vkQueueSubmit (lighting pass). Intentando recrear recursos...\n");
        recreateSwapchainAndResources();
        return;
    }
    VkPipelineStageFlags waitStagesPost[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSubmitInfo submitPost{};
    submitPost.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitPost.waitSemaphoreCount = 1;
    submitPost.pWaitSemaphores = &vkLightingToPostprocessSemaphore;
    submitPost.pWaitDstStageMask = waitStagesPost;
    submitPost.commandBufferCount = 1;
    submitPost.pCommandBuffers = &vkCommandBuffers[imageIndex];
    submitPost.signalSemaphoreCount = 1;
    submitPost.pSignalSemaphores = &vkRenderFinishedSemaphore;
    if (vkQueueSubmit(vkGraphicsQueue, 1, &submitPost, vkInFlightFence) != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan] Error en vkQueueSubmit (postprocess pass). Intentando recrear recursos...\n");
        recreateSwapchainAndResources();
        return;
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &vkRenderFinishedSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &vkSwapchain;
    presentInfo.pImageIndices = &imageIndex;
    presentInfo.pResults = nullptr;

    VkResult presentResult = vkQueuePresentKHR(vkPresentQueue, &presentInfo);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
        recreateSwapchainAndResources();
    } else if (presentResult == VK_ERROR_DEVICE_LOST) {
        fprintf(stderr, "[Vulkan] VK_ERROR_DEVICE_LOST en present. Intentando reinicializar contexto...\n");
        recreateSwapchainAndResources();
    } else if (presentResult != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan] Error inesperado en vkQueuePresentKHR: %d\n", presentResult);
        recreateSwapchainAndResources();
    }
    auto frameEnd = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> frameDuration = frameEnd - frameStart;
    g_lastFrameTimeMs = static_cast<float>(frameDuration.count());
    g_frameCount++;
    double now = std::chrono::duration<double>(frameEnd.time_since_epoch()).count();
    if (now - g_lastFpsTime > 1.0) {
        g_lastFps = static_cast<float>(g_frameCount / (now - g_lastFpsTime));
        g_lastFpsTime = now;
        g_frameCount = 0;
    }
    // Actualizar overlay de debug
    FrameMetrics metrics;
    metrics.fps = g_lastFps;
    metrics.frameTimeMs = g_lastFrameTimeMs;
    metrics.gpuTimeMs = g_lastGpuTimeMs;
    DebugOverlay::getInstance().updateMetrics(metrics);
}

// --- Implementación de recreación dinámica del swapchain y recursos dependientes ---
void Application::recreateSwapchainAndResources() {
        printf("[Haruka] >> INICIO recreateSwapchainAndResources\n");
        printf("[Haruka] << FIN recreateSwapchainAndResources\n");
        printf("[Haruka] >> INICIO cleanup\n");
        printf("[Haruka] << FIN cleanup\n");
    vkDeviceWaitIdle(vkDevice);
    // Destruir framebuffers
    for (auto fb : vkFramebuffers) {
        if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(vkDevice, fb, nullptr);
    }
    vkFramebuffers.clear();
    // Destruir image views
    for (auto view : vkSwapchainImageViews) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(vkDevice, view, nullptr);
    }
    vkSwapchainImageViews.clear();
    // Destruir swapchain
    if (vkSwapchain) vkDestroySwapchainKHR(vkDevice, vkSwapchain, nullptr);
    // Volver a crear swapchain y recursos dependientes
    // --- Re-crear swapchain ---
    VkSwapchainCreateInfoKHR swapchainCreateInfo{};
    swapchainCreateInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainCreateInfo.surface = vkSurface;
    swapchainCreateInfo.minImageCount = 2;
    swapchainCreateInfo.imageFormat = VK_FORMAT_B8G8R8A8_SRGB;
    swapchainCreateInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swapchainCreateInfo.imageExtent = { (uint32_t)_width, (uint32_t)_height };
    swapchainCreateInfo.imageArrayLayers = 1;
    swapchainCreateInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchainCreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainCreateInfo.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapchainCreateInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainCreateInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchainCreateInfo.clipped = VK_TRUE;
    swapchainCreateInfo.oldSwapchain = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(vkDevice, &swapchainCreateInfo, nullptr, &vkSwapchain) != VK_SUCCESS) {
        throw std::runtime_error("Fallo al recrear VkSwapchainKHR");
    }
    // --- Re-crear imágenes y image views ---
    uint32_t imageCount = 0;
    vkGetSwapchainImagesKHR(vkDevice, vkSwapchain, &imageCount, nullptr);
    vkSwapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(vkDevice, vkSwapchain, &imageCount, vkSwapchainImages.data());
    vkSwapchainImageViews.resize(imageCount);
    for (size_t i = 0; i < imageCount; ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = vkSwapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_B8G8R8A8_SRGB;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(vkDevice, &viewInfo, nullptr, &vkSwapchainImageViews[i]) != VK_SUCCESS) {
            throw std::runtime_error("Fallo al recrear VkImageView de swapchain");
        }
    }
    // --- Re-crear framebuffers ---
    vkFramebuffers.resize(vkSwapchainImageViews.size());
    for (size_t i = 0; i < vkSwapchainImageViews.size(); ++i) {
        VkImageView attachments[] = { vkSwapchainImageViews[i] };
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = vkRenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = _width;
        framebufferInfo.height = _height;
        framebufferInfo.layers = 1;
        if (vkCreateFramebuffer(vkDevice, &framebufferInfo, nullptr, &vkFramebuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("Fallo al recrear VkFramebuffer");
        }
    }
    // --- Regrabar command buffers ---
    for (size_t i = 0; i < vkCommandBuffers.size(); ++i) {
        vkResetCommandBuffer(vkCommandBuffers[i], 0);
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(vkCommandBuffers[i], &beginInfo);
        renderFrameContentVulkan(vkCommandBuffers[i], static_cast<uint32_t>(i));
        vkEndCommandBuffer(vkCommandBuffers[i]);
    }
}

// --- Lógica de escena y colas de render, independiente del backend ---
void Application::buildRenderQueue() {
    Haruka::Scene* scene = MotorInstance::getInstance().getScene();
    if (!scene) scene = _currentScene.get();
    g_sceneForRender = scene;
    g_sceneRenderQueue.clear();
    if (!scene) return;
    Camera* activeCamera = MotorInstance::getInstance().getCamera();
    if (!activeCamera) activeCamera = _camera.get();

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

    for (auto& obj : scene->getObjectsMutable()) {
        if (isRenderDisabledByEditor(obj)) continue;
        if (!isTerrainChunkFacingCamera(obj, activeCamera ? activeCamera->position : glm::dvec3(0.0))) continue;
        Haruka::ObjectType objType = Haruka::stringToObjectType(obj.type);
        if (!Haruka::isRenderableObjectType(objType)) continue;

        int layer = std::clamp(obj.renderLayer, 1, 5);
        const glm::dvec3& worldPos = obj.getWorldPosition(scene);
        const glm::dvec3& worldScale = obj.getWorldScale(scene);
        double maxScale = std::max(std::abs(worldScale.x), std::max(std::abs(worldScale.y), std::abs(worldScale.z)));
        const bool isHugeBody = (maxScale > 1000.0) || (obj.name == "Earth") || (obj.name == "Sun");

        if (!isHugeBody) {
            double unloadDistance = static_cast<double>(s_layerMaxDistance[layer]);
            double loadDistance = unloadDistance * 0.85;

            if (layer >= 4 && obj.meshRenderer) {
                glm::dvec3 diff = worldPos - camPos;
                double distSq = glm::dot(diff, diff);
                double unloadDistSq = unloadDistance * unloadDistance * 1.32;
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

        if (isHugeBody) {
            g_sceneRenderQueue.push_back(&obj);
            continue;
        }

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
}

void Application::renderFrameContentVulkan(VkCommandBuffer cmd, uint32_t imageIndex) {
    buildRenderQueue();
    Camera* activeCamera = MotorInstance::getInstance().getCamera();
    if (!activeCamera) activeCamera = _camera.get();
    if (!activeCamera) return;
    glm::mat4 proj = glm::perspective(glm::radians(activeCamera->zoom), (float)_width / (float)_height, 0.0001f, 300000000.0f);
    glm::mat4 view = activeCamera->getViewMatrix();
    struct ViewProjUBO {
        glm::mat4 view;
        glm::mat4 proj;
    } vpUbo;
    vpUbo.view = view;
    vpUbo.proj = proj;
    void* data;
    vkMapMemory(vkDevice, vkUniformBufferMemory, 0, sizeof(ViewProjUBO), 0, &data);
    memcpy(data, &vpUbo, sizeof(ViewProjUBO));
    vkUnmapMemory(vkDevice, vkUniformBufferMemory);

    // --- GEOMETRY PASS ---
    VkRenderPassBeginInfo geometryPassInfo{};
    geometryPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    geometryPassInfo.renderPass = vkRenderPass;
    geometryPassInfo.framebuffer = vkFramebuffers[imageIndex];
    geometryPassInfo.renderArea.offset = {0, 0};
    geometryPassInfo.renderArea.extent = { (uint32_t)_width, (uint32_t)_height };
    VkClearValue clearValues[2] = {};
    clearValues[0].color = { {0.0f, 0.0f, 0.0f, 1.0f} };
    clearValues[1].depthStencil = { 1.0f, 0 };
    geometryPassInfo.clearValueCount = 2;
    geometryPassInfo.pClearValues = clearValues;
    vkCmdBeginRenderPass(cmd, &geometryPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkGeometryPipeline);
    for (const auto* obj : g_sceneRenderQueue) {
        if (!obj) continue;
        glm::mat4 model = obj->getWorldTransform(g_sceneForRender);
        vkCmdPushConstants(cmd, vkGeometryPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &model);
        VkDescriptorSet descriptorSet = vkUniformDescriptorSet;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkGeometryPipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
        if (obj->meshRenderer && obj->meshRenderer->isResident()) {
            obj->meshRenderer->getMesh()->draw(cmd);
        } else if (!obj->modelPath.empty()) {
            try {
                auto modelPtr = getOrLoadModel(obj->modelPath);
                if (modelPtr) modelPtr->draw(cmd);
            } catch (...) {
                // Ignore model load/draw errors
            }
        }
    }
    vkCmdEndRenderPass(cmd);

    // --- LIGHTING PASS ---
    if (vkLightingTarget && vkLightingTarget->vkFramebuffer != VK_NULL_HANDLE) {
        VkRenderPassBeginInfo lightingPassInfo{};
        lightingPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        lightingPassInfo.renderPass = vkRenderPass;
        lightingPassInfo.framebuffer = vkLightingTarget->vkFramebuffer;
        lightingPassInfo.renderArea.offset = {0, 0};
        lightingPassInfo.renderArea.extent = { (uint32_t)_width, (uint32_t)_height };
        VkClearValue clearValuesLight[2] = {};
        clearValuesLight[0].color = { {0.0f, 0.0f, 0.0f, 1.0f} };
        clearValuesLight[1].depthStencil = { 1.0f, 0 };
        lightingPassInfo.clearValueCount = 2;
        lightingPassInfo.pClearValues = clearValuesLight;
        vkCmdBeginRenderPass(cmd, &lightingPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkLightingPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkLightingPipelineLayout, 0, 1, &vkUniformDescriptorSet, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    // --- POSTPROCESS PASS ---
    if (vkPostprocessTarget && vkPostprocessTarget->vkFramebuffer != VK_NULL_HANDLE) {
        VkRenderPassBeginInfo postprocessPassInfo{};
        postprocessPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        postprocessPassInfo.renderPass = vkRenderPass;
        postprocessPassInfo.framebuffer = vkPostprocessTarget->vkFramebuffer;
        postprocessPassInfo.renderArea.offset = {0, 0};
        postprocessPassInfo.renderArea.extent = { (uint32_t)_width, (uint32_t)_height };
        VkClearValue clearValuesPost[1] = {};
        clearValuesPost[0].color = { {0.0f, 0.0f, 0.0f, 1.0f} };
        postprocessPassInfo.clearValueCount = 1;
        postprocessPassInfo.pClearValues = clearValuesPost;
        vkCmdBeginRenderPass(cmd, &postprocessPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPostprocessPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPostprocessPipelineLayout, 0, 1, &vkUniformDescriptorSet, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    // --- PRESENT PASS (swapchain) ---
    VkClearValue clearColor = { {{0.1f, 0.1f, 0.1f, 1.0f}} };
    VkRenderPassBeginInfo presentPassInfo{};
    presentPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    presentPassInfo.renderPass = vkRenderPass;
    presentPassInfo.framebuffer = vkFramebuffers[imageIndex];
    presentPassInfo.renderArea.offset = {0, 0};
    presentPassInfo.renderArea.extent = { (uint32_t)_width, (uint32_t)_height };
    presentPassInfo.clearValueCount = 1;
    presentPassInfo.pClearValues = &clearColor;
    vkCmdBeginRenderPass(cmd, &presentPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPresentPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPresentPipelineLayout, 0, 1, &vkUniformDescriptorSet, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}
