#ifndef APPLICATION_H
#define APPLICATION_H

#include <glad/glad.h>
#include <SDL3/SDL.h>
#include <memory>
#include <vector>
#include <functional>
#include <chrono>

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
    
    /** @name Accessors */
    ///@{
    Camera* getCamera() { return _camera.get(); }
    Haruka::Scene* getCurrentScene() { return _currentScene.get(); }
    RaycastSimple* getRaycastSystem() { return _raycastSystem.get(); }
    Haruka::PlanetarySystem* getPlanetarySystem() { return _planetarySystem.get(); }
    ///@}

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
    void onSceneChanged(Haruka::Scene* scene) {
        if (!scene) {
            _currentScene.reset();
            return;
        }
        _currentScene = std::make_unique<Haruka::Scene>(*scene);
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

    /** @brief Starts runtime using a scene path bootstrap. */
    void run(const std::string& startScenePath);
    /** @brief Initializes systems from a scene instance. */
    void init(Haruka::Scene& scene);
    /** @brief Creates runtime window resources (when applicable). */
    void create_window();
    /** @brief Initializes the OpenGL context, GLAD, and input callbacks. */
    void create_gl_context();
    /** @brief Recreates all size-dependent FBOs when the viewport is resized. */
    void recreateFBOs(int newWidth, int newHeight);
    /** @brief Sets the viewport-owned FBO that the editor render path writes into.
     *  Must be called after recreateRenderTarget() on the viewport side. */
    void setEditorTarget(RenderTarget* rt) { _editorTarget = rt; }

    /** @brief Loads scene data from disk path. */
    void loadScene(const std::string& scenePath);
    /** @brief Builds the render queue with frustum culling and LOD management. */
    void buildRenderQueue();
    /** @brief Runs main loop until shutdown. */
    void main_loop();
    /** @brief Renders one frame and updates timing state. */
    void renderFrame();
    /** @brief Frame rendering body (logic-only path). */
    void renderFrameContent();
    /** @brief Releases allocated runtime resources. */
    void cleanup();

private:
    friend class MotorInstance;
    
    static constexpr int MAX_LIGHTS = 256;
    
    // Window (set by viewport via MotorInstance friend access)
    SDL_Window*   _window    = nullptr;
    SDL_GLContext _glContext = nullptr;
    int _width = 1280;
    int _height = 720;

    // Core systems
    std::unique_ptr<Haruka::Scene> _currentScene;
    std::unique_ptr<Camera> _camera;
    
    // Rendering pipeline
    std::unique_ptr<Shader> _mainShader;
    std::unique_ptr<Shader> _lampShader;
    std::unique_ptr<Shadow> _shadowSystem;
    std::unique_ptr<HDR> _hdrSystem;
    std::unique_ptr<Bloom> _bloomSystem;
    std::unique_ptr<GBuffer> _gBuffer;
    std::unique_ptr<SSAO> _ssaoSystem;
    std::unique_ptr<IBL> _iblSystem;
    std::unique_ptr<PointShadow> _pointShadowSystem;
    std::unique_ptr<LightCuller> _lightCuller;
    std::unique_ptr<GPUInstancing> _instancing;
    std::unique_ptr<ComputePostProcess> _computePostProcess;
    std::unique_ptr<CascadedShadowMap> _cascadedShadow;
    std::unique_ptr<VirtualTexturing> _virtualTexturing;
    std::unique_ptr<RaycastSimple> _raycastSystem;
    std::unique_ptr<Haruka::WorldSystem> _worldSystem;
    std::unique_ptr<Haruka::PlanetarySystem> _planetarySystem;
    std::unique_ptr<Haruka::TerrainStreamingSystem> _terrainStreamingSystem;
    
    // Editor viewport target — set explicitly by viewport, bypasses MotorInstance singleton split.
    RenderTarget* _editorTarget = nullptr;
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
    
    // ImGui injection callback (set by editor viewport)
    std::function<void()> _imguiCallback;

    float _exposure = 1.0f;

    // Timing (chrono — platform-independent, survives SDL3 migration)
    std::chrono::time_point<std::chrono::high_resolution_clock> _frameStart;
    float _lastFrameTimeMs = 0.0f;
    float _lastFps         = 0.0f;
    uint64_t _fpsFrameCount  = 0;
    double   _fpsLastTime    = 0.0;
    // Kept for Camera::processInput() compatibility
    float deltaTime = 0.0f;
    
    // Screen quad for post-processing
    unsigned int quadVAO = 0;
    unsigned int quadVBO = 0;
    void setupQuad();

    inline static int s_renderQualityPreset = 2; // 0=Low,1=Medium,2=High,3=Ultra
    inline static float s_layerMaxDistance[6] = {
        0.0f,
        1.0e9f,  // layer 1: always
        1200.0f, // layer 2: lowest details
        3500.0f, // layer 3: medium details / clouds
        900.0f,  // layer 4: buildings
        300.0f   // layer 5: small props
    };
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