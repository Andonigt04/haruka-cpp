#ifndef APPLICATION_H
#define APPLICATION_H


// Standard
#include <memory>
#include <vector>

// Vulkan
#include <vulkan/vulkan.h>

// SDL
#include <SDL3/SDL.h>

// Forward declarations (por si faltan en includes de proyecto)
class RenderTarget;
class Shadow;
class HDR;
class Bloom;
class GBuffer;
class SSAO;
class IBL;
class PointShadow;
class LightCuller;
class GPUInstancing;
class ComputePostprocess;
class CascadedShadow;
class VirtualTexturing;
class SimpleMesh;
class RaycastSimple;

namespace Haruka {
    class WorldSystem;
    class Scene;
    class PlanetarySystem;
    class TerrainStreamingSystem;
}

// Project
#include "math_types.h"
#include "world_system.h"
#include "camera.h"
#include "scene.h"
#include "renderer/shader.h"
#include "renderer/shadow.h"
#include "renderer/hdr.h"
#include "renderer/bloom.h"
#include "renderer/gbuffer.h"
#include "renderer/ssao.h"
#include "renderer/ibl.h"
#include "renderer/point_shadow.h"
#include "renderer/render_target.h"
#include "renderer/simple_mesh.h"
#include "renderer/light_culler.h"
#include "renderer/gpu_instancing.h"
#include "renderer/compute_postprocess.h"
#include "renderer/cascaded_shadow.h"
#include "renderer/virtual_texturing.h"
#include "error_reporter.h"
#include "io/asset_streamer.h"
#include "debug_overlay.h"
#include "physics/raycast_simple.h"
#include "core/terrain_streaming_system.h"

class MotorInstance;

/**
 * @brief Haruka runtime application orchestrator.
 *
 * Responsibilities:
 * - initialize render/scene systems
 * - render frames for the active scene
 * - maintain camera and global render state
 *
 * Non-responsibilities:
 * - window creation/ownership in editor-driven embedding mode
 * - high-level editor UI orchestration
 */

class Application {
public:
    Application();
    ~Application();

    // --- Vulkan ---
    /** @brief Inicializa contexto y swapchain Vulkan (experimental). */
    void create_vulkan_context();

    // --- Accesores y control ---
    Camera* getCamera() { return _camera.get(); }
    Haruka::Scene* getCurrentScene() { return _currentScene.get(); }
    RaycastSimple* getRaycastSystem() { return _raycastSystem.get(); }
    Haruka::PlanetarySystem* getPlanetarySystem() { return _planetarySystem.get(); }

    // Render quality/layers (global editor-configurable)
    static void setRenderQualityPreset(int preset) { s_renderQualityPreset = std::clamp(preset, 0, 3); }
    static int getRenderQualityPreset() { return s_renderQualityPreset; }
    static void setLayerMaxDistance(int layer, float distance) {
        if (layer < 1 || layer > 5) return;
        s_layerMaxDistance[layer] = std::max(0.0f, distance);
    }
    static float getLayerMaxDistance(int layer) {
        if (layer < 1 || layer > 5) return 0.0f;
        return s_layerMaxDistance[layer];
    }
    static int getLastRenderedVertices() { return s_lastRenderedVertices; }
    static int getLastRenderedTriangles() { return s_lastRenderedTriangles; }
    static int getLastRenderedDrawCalls() { return s_lastRenderedDrawCalls; }
    static int getLastTotalVertices() { return s_lastTotalVertices; }
    static int getLastTotalTriangles() { return s_lastTotalTriangles; }
    static int getLastTotalDrawCalls() { return s_lastTotalDrawCalls; }
    static int getLastVisibleChunks() { return s_lastVisibleChunks; }
    static int getLastResidentChunks() { return s_lastResidentChunks; }
    static int getLastPendingChunkLoads() { return s_lastPendingChunkLoads; }
    static int getLastPendingChunkEvictions() { return s_lastPendingChunkEvictions; }
    static int getLastResidentMemoryMB() { return s_lastResidentMemoryMB; }
    static int getLastTrackedChunks() { return s_lastTrackedChunks; }
    static int getLastMaxMemoryMB() { return s_lastMaxMemoryMB; }

    CascadedShadow* getCascadedShadowMap() { return _cascadedShadow.get(); }
    Shader* getCascadedShadowShader() { return _cascadeShadowShader.get(); }

    // Callbacks de integración
    void onSceneChanged(Haruka::Scene* scene) {
        if (!scene) { _currentScene.reset(); return; }
        _currentScene = std::make_unique<Haruka::Scene>(*scene);
    }
    void onCameraChanged(Camera* cam) {
        if (!cam) { _camera.reset(); return; }
        _camera = std::make_unique<Camera>(cam->position);
        _camera->orientation = cam->orientation;
        _camera->zoom = cam->zoom;
        _camera->speed = cam->speed;
        _camera->sensitivity = cam->sensitivity;
    }

    // Ciclo de vida principal
    void run(const std::string& startScenePath);
    void init(Haruka::Scene& scene);
    void create_window();
    void loadScene(const std::string& scenePath);
    void renderScene(Shader* shader = nullptr);
    void main_loop();
    void renderFrame();
    void renderFrameContent();
    void renderFrameContentVulkan(VkCommandBuffer cmd, uint32_t imageIndex);
    void recreateSwapchainAndResources();
    void cleanup();

private:
    // --- Vulkan pipeline/layout por pass (ejemplo: geometry, shadow, etc.) ---
    VkPipelineLayout vkGeometryPipelineLayout = VK_NULL_HANDLE;
    VkPipeline vkGeometryPipeline = VK_NULL_HANDLE;
    VkPipelineLayout vkLightingPipelineLayout = VK_NULL_HANDLE;
    VkPipeline vkLightingPipeline = VK_NULL_HANDLE;
    VkPipelineLayout vkPostprocessPipelineLayout = VK_NULL_HANDLE;
    VkPipeline vkPostprocessPipeline = VK_NULL_HANDLE;
    VkPipelineLayout vkPresentPipelineLayout = VK_NULL_HANDLE;
    VkPipeline vkPresentPipeline = VK_NULL_HANDLE;
    
    // --- Vulkan descriptor sets para materiales ---
    VkDescriptorSetLayout vkMaterialDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool vkDescriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> vkMaterialDescriptorSets;

    // --- Vulkan command pool y command buffers ---
    VkCommandPool vkCommandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> vkCommandBuffers;

    // --- Vulkan handles (experimental) ---
    VkInstance vkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice vkPhysicalDevice = VK_NULL_HANDLE;
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkSurfaceKHR vkSurface = VK_NULL_HANDLE;
    VkSwapchainKHR vkSwapchain = VK_NULL_HANDLE;
    std::vector<VkImage> vkSwapchainImages;
    std::vector<VkImageView> vkSwapchainImageViews;
    VkQueue vkGraphicsQueue = VK_NULL_HANDLE;
    VkQueue vkPresentQueue = VK_NULL_HANDLE;
    VkSemaphore vkImageAvailableSemaphore = VK_NULL_HANDLE;
    VkSemaphore vkRenderFinishedSemaphore = VK_NULL_HANDLE;
    VkFence vkInFlightFence = VK_NULL_HANDLE;

    // --- Vulkan render pass y framebuffers ---
    VkRenderPass vkRenderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> vkFramebuffers;

    // --- Core systems ---
    SDL_Window* _window = nullptr;
    int _width = 1280;
    int _height = 720;
    std::unique_ptr<Haruka::Scene> _currentScene;
    std::unique_ptr<Camera> _camera;

    // --- Rendering pipeline ---
    std::unique_ptr<Shader> _mainShader;
    std::unique_ptr<Shader> _lampShader;
    std::unique_ptr<Shadow> _shadowSystem;
    std::unique_ptr<HDR> _hdrSystem;
    std::unique_ptr<Bloom> _bloomSystem;
    std::unique_ptr<GBuffer> _gBuffer;
    std::unique_ptr<SSAO> _ssaoSystem;
    std::unique_ptr<IBL> _iblSystem;
    std::unique_ptr<PointShadow> _pointShadowSystem;
    std::unique_ptr<Haruka::WorldSystem> _worldSystem;
    std::unique_ptr<LightCuller> _lightCuller;
    std::unique_ptr<GPUInstancing> _instancing;
    std::unique_ptr<ComputePostprocess> _computePostProcess;
    std::unique_ptr<CascadedShadow> _cascadedShadow;
    std::unique_ptr<VirtualTexturing> _virtualTexturing;
    std::unique_ptr<Haruka::PlanetarySystem> _planetarySystem;
    std::unique_ptr<RaycastSimple> _raycastSystem;
    std::unique_ptr<Haruka::TerrainStreamingSystem> _terrainStreamingSystem;

    // --- Render targets ---
    std::unique_ptr<RenderTarget> _lightingTarget;
    std::unique_ptr<RenderTarget> _bloomExtractTarget;

    // --- Render targets Vulkan para passes principales ---
    std::unique_ptr<RenderTarget> vkGeometryTarget;
    std::unique_ptr<RenderTarget> vkLightingTarget;
    std::unique_ptr<RenderTarget> vkSSAOTarget;

    // --- Render targets Vulkan para todos los passes ---
    std::unique_ptr<RenderTarget> vkShadowTarget;
    std::unique_ptr<RenderTarget> vkBloomTarget;
    std::unique_ptr<RenderTarget> vkPostprocessTarget;

    // --- Sincronización Vulkan entre passes ---
    VkSemaphore vkShadowToGeometrySemaphore = VK_NULL_HANDLE;
    VkSemaphore vkGeometryToLightingSemaphore = VK_NULL_HANDLE;
    VkSemaphore vkLightingToPostprocessSemaphore = VK_NULL_HANDLE;

    // --- Primitives (LOD spheres for celestial bodies) ---
    std::unique_ptr<SimpleMesh> sphereLOD[4];

    // --- Shaders (cached to avoid recreation every frame) ---
    std::unique_ptr<Shader> _geomShader;
    std::unique_ptr<Shader> _ssaoShader;
    std::unique_ptr<Shader> _lightShader;
    std::unique_ptr<Shader> _compositeShader;
    std::unique_ptr<Shader> _flatShader;
    std::unique_ptr<Shader> _cascadeShadowShader;

    // --- Vulkan uniform buffer para matrices globales (MVP) ---
    VkBuffer vkUniformBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vkUniformBufferMemory = VK_NULL_HANDLE;
    VkDescriptorSetLayout vkUniformDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet vkUniformDescriptorSet = VK_NULL_HANDLE;

    // --- Timing ---
    float deltaTime = 0.0f;
    float lastFrame = 0.0f;

    // --- Screen quad for post-processing ---
    unsigned int quadVAO = 0;
    unsigned int quadVBO = 0;
    void setupQuad();

    // --- Render quality/statics ---
    inline static int s_renderQualityPreset = 2; // 0=Low,1=Medium,2=High,3=Ultra
    inline static float s_layerMaxDistance[6] = {
        0.0f,
        1.0e9f,  // layer 1: always
        1200.0f, // layer 2: lowest details
        3500.0f, // layer 3: medium details / clouds
        900.0f,  // layer 4: buildings
        300.0f   // layer 5: small props
    };
    inline static int s_lastRenderedVertices = 0;
    inline static int s_lastRenderedTriangles = 0;
    inline static int s_lastRenderedDrawCalls = 0;
    inline static int s_lastTotalVertices = 0;
    inline static int s_lastTotalTriangles = 0;
    inline static int s_lastTotalDrawCalls = 0;
    inline static int s_lastVisibleChunks = 0;
    inline static int s_lastResidentChunks = 0;
    inline static int s_lastPendingChunkLoads = 0;
    inline static int s_lastPendingChunkEvictions = 0;
    inline static int s_lastResidentMemoryMB = 0;
    inline static int s_lastTrackedChunks = 0;
    inline static int s_lastMaxMemoryMB = 0;

    // --- Métricas y queries de GPU ---
    VkQueryPool g_gpuQueryPool = VK_NULL_HANDLE;
    uint64_t g_gpuTimestamps[2] = {0, 0};
    float g_lastGpuTimeMs = 0.0f;
    uint64_t g_frameCount = 0;
    double g_lastFpsTime = 0.0;
    float g_lastFps = 0.0f;
    float g_lastFrameTimeMs = 0.0f;

    void buildRenderQueue();
};

#endif