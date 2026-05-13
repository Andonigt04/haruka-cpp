#ifndef APPLICATION_H
#define APPLICATION_H

#include <glad/glad.h>
#include <SDL3/SDL.h>
#include <memory>
#include <vector>
#include <functional>
#include <chrono>
#include <algorithm>

#ifdef HARUKA_NETWORK
    #include "include/dgs/client.h"
#endif

#include "tools/math_types.h"
#include "core/world_system.h"
#include "core/window.h"
#include "core/camera.h"
#include "core/scene/scene_manager.h"
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
#include "tools/error_reporter.h"
#include "io/asset_streamer.h"
#include "tools/debug_overlay.h"
#include "physics/raycast_simple.h"
#include "physics/physics_engine.h"
#include "core/terrain/terrain_streaming_system.h"
#include "core/scene/scene_loader.h"
#include "core/chunk_cache.h"

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
    
    /** @name Accessors */
    ///@{
    Camera* getCamera() { return _camera.get(); }
    Haruka::SceneManager* getCurrentScene() { return _currentScene; }
    RaycastSimple* getRaycastSystem() { return _raycastSystem.get(); }
    Haruka::PlanetarySystem* getPlanetarySystem() { return _planetarySystem.get(); }
    Haruka::PhysicsEngine* getPhysicsEngine() { return _physicsEngine.get(); }
    Haruka::ChunkCache* getChunkCache() { return _chunkCache.get(); }
    Haruka::TerrainStreamingSystem* getTerrainStreamingSystem() { return _terrainStreamingSystem.get(); }
    Haruka::WorldSystem* getWorldSystem() { return _worldSystem.get(); }
    ///@}

    /** @brief Sets the render quality preset. */
    static void setRenderQualityPreset(int preset) { s_renderQualityPreset = std::clamp(preset, 0, 3); }
    static int getRenderQualityPreset() { return s_renderQualityPreset; }
    static void setRenderFeatureHDR(bool enabled) { s_enableHDR = enabled; }
    static bool getRenderFeatureHDR() { return s_enableHDR; }
    static void setRenderFeatureBloom(bool enabled) { s_enableBloom = enabled; }
    static bool getRenderFeatureBloom() { return s_enableBloom; }
    static void setRenderFeatureSSAO(bool enabled) { s_enableSSAO = enabled; }
    static bool getRenderFeatureSSAO() { return s_enableSSAO; }
    static void setRenderFeatureIBL(bool enabled) { s_enableIBL = enabled; }
    static bool getRenderFeatureIBL() { return s_enableIBL; }
    static void setRenderFeatureShadows(bool enabled) { s_enableShadows = enabled; }
    static bool getRenderFeatureShadows() { return s_enableShadows; }

    /** @brief Sets the maximum distance for a render layer. */
    static void setLayerMaxDistance(int layer, float distance) {
        if (layer < 1 || layer > 5) return;
        s_layerMaxDistance[layer] = std::max(0.0f, distance);
    }
    /** @brief Gets the maximum distance for a render layer. */
    static float getLayerMaxDistance(int layer) {
        if (layer < 1 || layer > 5) return 0.0f;
        return s_layerMaxDistance[layer];
    }
    
    int getRenderedVertices()      const { return _iRenderedVertices; }
    int getRenderedTriangles()     const { return _iRenderedTriangles; }
    int getRenderedDrawCalls()     const { return _iRenderedDrawCalls; }
    int getTotalVertices()         const { return _iTotalVertices; }
    int getTotalTriangles()        const { return _iTotalTriangles; }
    int getTotalDrawCalls()        const { return _iTotalDrawCalls; }
    int getVisibleChunks()         const { return _iVisibleChunks; }
    int getResidentChunks()        const { return _iResidentChunks; }
    int getPendingChunkLoads()     const { return _iPendingChunkLoads; }
    int getPendingChunkEvictions() const { return _iPendingChunkEvictions; }
    int getResidentMemoryMB()      const { return _iResidentMemoryMB; }
    int getTrackedChunks()         const { return _iTrackedChunks; }
    int getMaxMemoryMB()           const { return _iMaxMemoryMB; }

    CascadedShadowMap* getCascadedShadowMap() { return _cascadedShadow.get(); }
    Shader* getCascadedShadowShader() { return _cascadeShadowShader.get(); }

    void setImGuiRenderCallback(std::function<void()> cb) { _imguiCallback = std::move(cb); }
    
    /**
     * @brief Callback invoked by `MotorInstance` when active scene changes.
     * @note Performs internal copy into owned scene storage.
     */
    void onSceneChanged(Haruka::SceneManager* scene) {
        _currentScene = scene;
    }
    /**
     * @brief Callback invoked by `MotorInstance` when viewport camera changes.
     * @note Copies camera state into local owned camera instance.
     */
    void onCameraChanged(Camera* cam) {
        if (!cam) {
            _camera.reset();
            return;
        }
        _camera = std::make_unique<Camera>(cam->position);
        _camera->orientation = cam->orientation;
        _camera->zoom = cam->zoom;
        _camera->speed = cam->speed;
        _camera->sensitivity = cam->sensitivity;
    }

#ifdef HARUKA_NETWORK
    void sendPlayerTransform(uint32_t uuid, const Haruka::WorldPos& pos, const Haruka::Rotation& rot);
#endif

    /** @brief Starts runtime using a scene path bootstrap. */
    void run(const std::string& startScenePath);
    /** @brief Initializes systems from a scene instance. */
    void init(Haruka::SceneManager& scene);
    /** @brief Recreates all size-dependent FBOs when the viewport is resized. */
    void recreateFBOs(int newWidth, int newHeight);
    /** @brief Sets the viewport-owned FBO that the editor render path writes into.
     *  Must be called after recreateRenderTarget() on the viewport side. */
    void setEditorTarget(RenderTarget* rt) { _editorTarget = rt; }
    /** @brief Sets the render size used in editor mode (when no Window exists). */
    void setEditorViewportSize(int w, int h);
    /** @brief Scans current scene and initialises PlanetarySystem with planet objects. */
    void initPlanetarySystem();
    /** @brief Loads scene data from disk path. */
    void loadScene(const std::string& scenePath);
    /** @brief Builds the render queue with frustum culling and LOD management. */
    void buildRenderQueue();
    /** @brief Renders one frame and updates timing state. */
    void renderFrame();
    /** @brief Frame rendering body (logic-only path). */
    void renderFrameContent();
    /** @brief Releases allocated runtime resources. */
    void cleanup();

private:
    friend class MotorInstance;
    
    /** @brief The main application window. */
    std::unique_ptr<Haruka::Core::Window> _window = nullptr;
    /** @brief The currently active scene. */
    Haruka::SceneManager* _currentScene = nullptr;
    /** @brief The owned scene instance. */
    std::unique_ptr<Haruka::SceneManager> _ownedScene;
    /** @brief The active camera instance. */
    std::unique_ptr<Camera> _camera;
    
    /** @brief The main shader instance. */
    std::unique_ptr<Shader> _mainShader;
    /** @brief The lamp shader instance. */
    std::unique_ptr<Shader> _lampShader;
    /** @brief The shadow shader instance. */
    std::unique_ptr<Shadow> _shadow;
    /** @brief The HDR shader instance. */
    std::unique_ptr<HDR> _hdr;
    /** @brief The bloom shader instance. */
    std::unique_ptr<Bloom> _bloom;
    /** @brief The G-buffer shader instance. */
    std::unique_ptr<GBuffer> _gBuffer;
    /** @brief The SSAO shader instance. */
    std::unique_ptr<SSAO> _ssao;
    /** @brief The IBL shader instance. */
    std::unique_ptr<IBL> _ibl;
    /** @brief The point shadow shader instance. */
    std::unique_ptr<PointShadow> _pointShadow;
    /** @brief The light culler instance. */
    std::unique_ptr<LightCuller> _lightCuller;
    /** @brief The GPU instancing instance. */
    std::unique_ptr<GPUInstancing> _instancing;
    /** @brief The compute post-process instance. */
    std::unique_ptr<ComputePostProcess> _computePostProcess;
    /** @brief The cascaded shadow map instance. */
    std::unique_ptr<CascadedShadowMap> _cascadedShadow;
    /** @brief The virtual texturing instance. */
    std::unique_ptr<VirtualTexturing> _virtualTexturing;
    /** @brief The raycast system instance. */
    std::unique_ptr<RaycastSimple> _raycastSystem;
    /** @brief The world system instance. */
    std::unique_ptr<Haruka::WorldSystem> _worldSystem;
    /** @brief The planetary system instance. */
    std::unique_ptr<Haruka::PlanetarySystem> _planetarySystem;
    /** @brief The terrain streaming system instance. */
    std::unique_ptr<Haruka::TerrainStreamingSystem> _terrainStreamingSystem;
    /** @brief The physics engine instance. */
    std::unique_ptr<Haruka::PhysicsEngine> _physicsEngine;
    /** @brief The chunk cache instance. */
    std::unique_ptr<Haruka::ChunkCache> _chunkCache;
    
    // Editor viewport target — set explicitly by viewport, bypasses MotorInstance singleton split.
    RenderTarget* _editorTarget = nullptr;
    // Editor viewport size (used when no _window exists).
    int m_editorViewportW = 0;
    int m_editorViewportH = 0;
    // Resets to 0 on each init(); renderFrameContent logs the first 5 frames per init.
    int _diagFramesLeft = 0;

    // Render targets
    std::unique_ptr<RenderTarget> _lightingTarget;
    std::unique_ptr<RenderTarget> _bloomExtractTarget;
    std::unique_ptr<RenderTarget> _bloomPing;
    std::unique_ptr<RenderTarget> _bloomPong;
    
    // Primitives (LOD spheres for celestial bodies)
    std::unique_ptr<SimpleMesh> sphereLOD[4];
    std::unique_ptr<SimpleMesh> _testCube;
    
    // Shaders (cached to avoid recreation every frame)
    std::unique_ptr<Shader> _geomShader;
    std::unique_ptr<Shader> _ssaoShader;
    std::unique_ptr<Shader> _lightShader;
    std::unique_ptr<Shader> _compositeShader;
    std::unique_ptr<Shader> _flatShader;
    std::unique_ptr<Shader> _cascadeShadowShader;
    std::unique_ptr<Shader> _bloomExtractShader;
    std::unique_ptr<Shader> _bloomBlurShader;
    std::unique_ptr<Shader> _pointShadowShader;
    std::unique_ptr<Shader> _instancingShader;
    bool _mainShaderUsesFinalLook = false;
    
    // ImGui injection callback (set by editor viewport)
    std::function<void()> _imguiCallback;

    float _exposure = 1.0f;

    /** @brief Timing state for frame time and FPS calculation. Updated in renderFrame(). */
    std::chrono::time_point<std::chrono::high_resolution_clock> _frameStart;
    float _lastFrameTimeMs = 0.0f;
    float _lastFps         = 0.0f;
    uint64_t _fpsFrameCount  = 0;
    double   _fpsLastTime    = 0.0;

    /** @brief The time elapsed since the last frame. */
    float deltaTime = 0.0f;
    
    /** @brief The vertex array object for the screen quad. */
    unsigned int quadVAO = 0;
    /** @brief The vertex buffer object for the screen quad. */
    unsigned int quadVBO = 0;
    /** @brief Sets up the screen quad for post-processing. */
    void setupQuad();

    #ifdef HARUKA_NETWORK
        DGS::Client m_dgs;
    #endif

    // UBOs shared by all forward-rendering shaders (bindings 0 and 1)
    unsigned int m_uboPerFrame  = 0;
    unsigned int m_uboPerObject = 0;

    /** @brief The render quality preset. */
    inline static int s_renderQualityPreset = 2; // 0=Low,1=Medium,2=High,3=Ultra
    inline static bool s_enableHDR = true;
    inline static bool s_enableBloom = true;
    inline static bool s_enableSSAO = true;
    inline static bool s_enableIBL = true;
    inline static bool s_enableShadows = true;

    inline static float s_layerMaxDistance[6] = {
        0.0f,
        1.0e9f,  // layer 1: always
        1200.0f, // layer 2: lowest details
        3500.0f, // layer 3: medium details / clouds
        900.0f,  // layer 4: buildings
        300.0f   // layer 5: small props
    };

    /** @brief Statistics for rendered geometry. */
    int _iRenderedVertices      = 0;
    int _iRenderedTriangles     = 0;
    int _iRenderedDrawCalls     = 0;
    int _iTotalVertices         = 0;
    int _iTotalTriangles        = 0;
    int _iTotalDrawCalls        = 0;
    int _iVisibleChunks         = 0;
    int _iResidentChunks        = 0;
    int _iPendingChunkLoads     = 0;
    int _iPendingChunkEvictions = 0;
    int _iTrackedChunks         = 0;
    int _iResidentMemoryMB      = 0;
    int _iMaxMemoryMB           = 0;
};

#endif