// Application — lifecycle & orchestration.
// Construction/teardown, scene loading, planetary-system bring-up, graphics
// settings application, FBO (re)creation and the standalone main loop. The
// per-frame rendering lives in application_render.cpp, the GL asset caches in
// application_assets.cpp and the DGS network bridge in application_network.cpp.

#include "application.h"
#include "application_internal.h"

#include <iostream>
#include <algorithm>
#include <csignal>
#include <atomic>
#include <filesystem>
#include <cstring>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>

#include "renderer/motor_instance.h"
#include "game/planetary_system.h"
#include "core/terrain/terrain_generator.h"
#include "renderer/texture.h"
#include "core/scene/scene_loader.h"
#include "tools/error_reporter.h"
#include "tools/profiler.h"
#include "settings/settings_manager.h"

namespace Haruka { namespace Core {

static std::atomic<bool> g_sigintReceived{false};
static void handleSigint(int) { g_sigintReceived.store(true); }

Application::Application() : _window(nullptr) {
    _frameStart = std::chrono::high_resolution_clock::now();
}

Application::~Application() {
    cleanup();
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

    // Wire up planetary physics: la física solo conoce IWorldProvider (sin GL). Aquí se le inyecta
    // el adaptador CLIENTE sobre WorldSystem (gravedad + mar) + PlanetarySystem (altura del terreno).
#ifdef HARUKA_MOD_PHYSICS
    if (_physicsEngine) {
        _worldProvider = std::make_unique<Haruka::WorldSystemProvider>(
            _worldSystem.get(), _planetarySystem.get());
        _physicsEngine->setWorldProvider(_worldProvider.get());
    }
#endif

    // Toda la interpretación escena→cuerpos celestes (planetas con terreno + cuerpos
    // con malla procedural como la Luna) vive en PlanetarySystem.
    _planetarySystem->buildFromScene(*_currentScene);
}

void Application::applyGraphicsSettings() {
    const auto& g = Haruka::SettingsManager::get().graphics();

    setRenderFeatureBloom(g.bloom);
    setRenderFeatureSSAO(g.ssao);
    setRenderFeatureShadows(g.shadowQuality != Haruka::Settings::ShadowQuality::Off);
    setRenderQualityPreset(static_cast<int>(g.shadowQuality)); // 0=Off/Low..3=High

    if (_camera)
        _camera->zoom = g.fov;

    if (_window) {
        SDL_GL_SetSwapInterval(g.vsync ? 1 : 0);
        _window->setWindowMode(static_cast<int>(g.windowMode)); // windowed / borderless / fullscreen
    }

    // Chunk cache memory budget.
    if (_planetarySystem)
        _planetarySystem->setCacheMaxMemoryMB(g.chunkMemoryMB);

    // Terrain LOD detail: lower = fewer/larger chunks = cheaper (CPU/GPU/RAM).
    // Also scales the per-vertex noise octaves (cheaper chunk generation).
    if (_planetarySystem) {
        switch (g.terrainQuality) {
            // chunkSize=96 (scene): la malla de AGUA usa esta res y a 96 la costa lejana queda
            // CONTINUA (a 64 aliasaba en tiles = "el círculo"; a 48 en celdas/discos). El coste es
            // RAM de caché (96²=9216 quads/chunk); con 32GB no es problema si el cap de chunkMemory
            // está alto (0=Auto → ~33% RAM). maxLOD 15/17/18/19. Ajuste en vivo con `terrainq`.
            // Ver perf_terrain_chunksize_rootcause (memoria). Fix definitivo pendiente = decouplar
            // la res del agua del terreno (agua fina + terreno más barato).
            // LOD ADAPTATIVO: el preset fija el PRESUPUESTO de frame (1000/fpsObjetivo RX 6600: low 300 /
            // mid 260 / high 160 / ultra 100) y las COTAS de calidad [fino,grueso]. El targetPx se ajusta
            // solo hacia la mejor calidad que ese presupuesto sostiene; solo engorda (menos detalle) si el
            // frame se pasa. Así el LOD degrada SOLO bajo carga (no es un tope fijo agresivo).
            // Cota GRUESA (maxPx) ACOTADA a un LOD que NUNCA muestre triángulos grandes: coarsear más
            // allá de esto NO sube FPS cuando el cuello es CPU (GPU ociosa) — solo degrada el detalle en
            // balde. Rango ESTRECHO min↔max → el adaptativo apenas oscila (sin "respirar" de detalle al
            // girar). Bajo carga real el terreno se queda fino; solo baja un pelín, sin verse facetado.
            // Rangos afinados con el barrido de calidad del harness (test_quality_sweep): la curva
            // calidad/targetPx va en ESCALONES; el salto grande de detalle está en ~280 (700-380 no
            // gana nada). Bajamos el rango para ALCANZAR ese escalón → casi el doble de detalle por
            // ~1 ms, sin desperdiciar LOD en la zona plana. El adaptativo engorda hacia el max bajo carga.
            case Haruka::Settings::TerrainQuality::Low:    _planetarySystem->setLODParams(0.70, 15); _planetarySystem->setLODBudget(1000.0/300.0, 280.0, 400.0); Haruka::TerrainGenerator::s_detailScale = 0.5f;  break;
            case Haruka::Settings::TerrainQuality::Medium: _planetarySystem->setLODParams(0.85, 17); _planetarySystem->setLODBudget(1000.0/260.0, 240.0, 340.0); Haruka::TerrainGenerator::s_detailScale = 0.75f; break;
            case Haruka::Settings::TerrainQuality::High:   _planetarySystem->setLODParams(1.00, 18); _planetarySystem->setLODBudget(1000.0/160.0, 180.0, 280.0); Haruka::TerrainGenerator::s_detailScale = 1.0f;  break;
            case Haruka::Settings::TerrainQuality::Ultra:  _planetarySystem->setLODParams(1.20, 19); _planetarySystem->setLODBudget(1000.0/100.0, 140.0, 220.0); Haruka::TerrainGenerator::s_detailScale = 1.0f;  break;
        }
        // OVERRIDE DE USUARIO (opción persistente): el preset fija el presupuesto adaptativo, pero el
        // usuario puede sobreponerse — adaptiveLOD=false congela el auto-ajuste, y lodTargetPx>0 fija un
        // detalle manual (px del split screen-space). Así "calidad automática según HW" es el default,
        // pero quien quiera manda el valor a mano.
        _planetarySystem->setAdaptiveLOD(g.adaptiveLOD);
        if (g.lodTargetPx > 0) _planetarySystem->setLODTargetPx((double)g.lodTargetPx);
        // CLAVE (perf): el LOD por defecto es SCREEN-SPACE (splitea si el chunk proyecta > targetPx).
        // Antes targetPx era FIJO a 320 para TODOS los presets → Low dibujaba los mismos ~1500 chunks
        // que Ultra → los presets NO cambiaban el nº de draws (el cuello es CPU draw-calls, GPU ociosa).
        // Ahora targetPx sube en Low (chunks más grandes en pantalla = MENOS draws = más FPS) y baja en
        // Ultra (más detalle). Runtime, sin mundo nuevo. Ajuste fino en vivo: `lodscreen on <px>`.
    }

    // Texture quality → anisotropic filtering + mip LOD bias. Low trades sharpness
    // for fill-rate (positive bias = blurrier mips); Ultra = max anisotropy, sharp.
    // Applies to textures loaded after this call (see Texture::setQuality).
    switch (g.textureQuality) {
        case Haruka::Settings::TextureQuality::Low:    Texture::setQuality(1.0f,  +1.0f); break;
        case Haruka::Settings::TextureQuality::Medium: Texture::setQuality(4.0f,   0.0f); break;
        case Haruka::Settings::TextureQuality::High:   Texture::setQuality(8.0f,   0.0f); break;
        case Haruka::Settings::TextureQuality::Ultra:  Texture::setQuality(16.0f, -0.5f); break;
    }
}

void Application::init(Haruka::SceneManager& scene) {
    _currentScene = &scene;
    applyGraphicsSettings();

    if (!_worldSystem) {
        _worldSystem = std::make_unique<Haruka::WorldSystem>();
        _worldSystem->init();
    }
    _worldSystem->syncFromScene(scene);

#ifdef HARUKA_MOD_PHYSICS
    if (!_physicsEngine) {
        _physicsEngine = std::make_unique<Haruka::PhysicsEngine>();
    }
#endif

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

    // 1. Update OpenGL's global viewport
    glViewport(0, 0, width, height);

    // 2. Recreate the G-Buffer (essential for deferred rendering)
    // The G-Buffer holds albedo, normal, position, etc. textures.
    _gBuffer = std::make_unique<GBuffer>(width, height);

    // 3. Recreate lighting + post-processing buffers
    _hdr = std::make_unique<HDR>(width, height);
    _bloom = std::make_unique<Bloom>(width, height);

    // 4. Recreate SSAO (needs the new size for its noise and samples)
    if (_ssao) {
        _ssao = std::make_unique<SSAO>(width, height);
    }

    // 5. Update the camera projection matrix
    if (_camera) {
        _camera->setAspectRatio((float)width / (float)height);
    }
}

void Application::cleanup() {
    if (m_cleanedUp) return;
    m_cleanedUp = true;

    AppInternal::g_sceneRenderQueue.clear();

#ifdef HARUKA_NETWORK
    m_dgs.disconnect();
#endif

    MotorInstance::getInstance().clear();

    _currentScene = nullptr;
    _ownedScene.reset();

#ifdef HARUKA_MOD_PHYSICS
    _physicsEngine.reset();
#endif
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
    _lampShader.reset();
    _geomShader.reset();
    _ssaoShader.reset();
    _lightShader.reset();
    _flatShader.reset();
    _cascadeShadowShader.reset();
    // POSTFX (ruta PSO — bloom + present): sus recursos los posee el RHI, no nosotros. Antes se
    // borraban los FBO y las texturas del bloom con glDelete* crudos aunque pertenecen a los render
    // targets del device (m_bloomPass) → doble-free / entrada huérfana en el pool. Ahora todo se
    // libera por el device.
    if (RHI::Device* dev = RHI::device()) {
        if (RHI::valid(m_bloomExtractPSO)) { dev->destroy(m_bloomExtractPSO); m_bloomExtractPSO = {}; }
        if (RHI::valid(m_bloomBlurPSO))    { dev->destroy(m_bloomBlurPSO);    m_bloomBlurPSO    = {}; }
        if (RHI::valid(m_bloomUBO))        { dev->destroy(m_bloomUBO);        m_bloomUBO        = {}; }
        if (RHI::valid(m_presentPSO))      { dev->destroy(m_presentPSO);      m_presentPSO      = {}; }
        if (RHI::valid(m_presentUBO))      { dev->destroy(m_presentUBO);      m_presentUBO      = {}; }
        if (RHI::valid(m_skyPSO))          { dev->destroy(m_skyPSO);          m_skyPSO          = {}; }
        if (RHI::valid(m_skyUBO))          { dev->destroy(m_skyUBO);          m_skyUBO          = {}; }
        if (RHI::valid(m_scenePSO))        { dev->destroy(m_scenePSO);        m_scenePSO        = {}; }
        for (int i = 0; i < 2; ++i)
            if (RHI::valid(m_bloomPass[i])) { dev->destroy(m_bloomPass[i]); m_bloomPass[i] = {}; }
    }
    m_bloomFBO[0] = m_bloomFBO[1] = 0;   // eran ids GL cacheados del pass (ya liberado arriba)
    m_bloomTexH[0] = m_bloomTexH[1] = {};
    m_bloomW = m_bloomH = 0;
    _postScene.reset();
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
    AppInternal::cleanupGLStatics();

    // CIERRE DEL RHI — el orden importa y estaba mal:
    //  1) setDevice(nullptr): el global g_device dejaba de actualizarse y quedaba COLGANDO al morir
    //     _device. Cualquier destructor de recurso posterior (Mesh/Texture llaman a
    //     device()->destroy()) invocaba un virtual sobre memoria liberada → "pure virtual method
    //     called". Ahora device() devuelve null y esos destructores son no-op (sus objetos GL ya
    //     los libera el barrido de pools del device, justo debajo).
    //  2) _device.reset() AQUÍ, no en ~Application(): el device se destruía DESPUÉS de que
    //     _window->shutdown() matara el contexto GL → sus glDelete* corrían sobre un contexto
    //     muerto. Destruyéndolo antes, el contexto sigue vivo y libera todo limpiamente.
    Haruka::RHI::setDevice(nullptr);
    _device.reset();

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
        // Título de la ventana = nombre del juego; icono desde assets/icons/icon.png (si existe).
        Haruka::Core::WindowProps("Survival", _width, _height, "assets/icons/icon.png")
    );
    if (!_window->init()) {
        HARUKA_MOTOR_ERROR(ErrorCode::WINDOW_CREATION_FAILED, "Failed to initialize Window system.");
        return;
    }

    // RHI: crea el device sobre la ventana. La Window ya creó y activó el contexto GL, así que
    // el backend GL lo ADOPTA (no crea uno segundo). Lo publicamos como device global para que
    // los wrappers (Texture, …) lo usen.
    //
    // El backend se elige por el setting persistente RenderBackend (menu de settings → imgui.ini).
    // Cambiarlo REQUIERE REINICIAR (el device se crea aquí, una vez). Vulkan cae a OpenGL si no está.
    // ⚠️ ORDEN (F5): este punto corre ANTES de que el juego cargue su imgui.ini (SettingsManager::init
    // va tras ImGui::CreateContext, más abajo) → hoy lee el DEFAULT. Inofensivo mientras Vulkan no exista
    // (fallback a GL). Cuando se implemente Vulkan, mover la carga de settings ANTES de esta línea.
    const auto& gfx = Haruka::SettingsManager::get().graphics();
    const Haruka::RHI::Backend requestedBackend =
        (gfx.renderBackend == Haruka::Settings::RenderBackend::Vulkan)
            ? Haruka::RHI::Backend::Vulkan : Haruka::RHI::Backend::OpenGL;
    _device = Haruka::RHI::Device::create(requestedBackend, _window->getNativeWindow());
    Haruka::RHI::setDevice(_device.get());

    // Log del backend ACTIVO vs SOLICITADO → deja claro si corre directo o cayó al fallback.
    if (_device) {
        auto beName = [](Haruka::RHI::Backend b) {
            return b == Haruka::RHI::Backend::OpenGL ? "OpenGL" : "Vulkan";
        };
        if (_device->backend() == requestedBackend)
            std::fprintf(stderr, "[RHI] Backend activo: %s (solicitado, sin fallback).\n", beName(_device->backend()));
        else
            std::fprintf(stderr, "[RHI] Backend activo: %s (FALLBACK desde %s).\n",
                         beName(_device->backend()), beName(requestedBackend));
    } else {
        std::fprintf(stderr, "[RHI] No hay device — el motor correrá por las rutas GL directas de compatibilidad.\n");
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
                // Update the window's stored size so getWidth()/getHeight() reflect
                // reality — otherwise the recreateFBOs() check below never fires and
                // the scene keeps rendering at the old resolution (viewport, FBOs and
                // camera aspect all stay stale).
                _window->setWidth(event.window.data1);
                _window->setHeight(event.window.data2);
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

        // HARUKA_PROF_LOG=N → vuelca el árbol del profiler a stderr cada N frames. Permite medir
        // el coste de un cambio sin depender de leer el panel en una captura.
        if (const char* pl = getenv("HARUKA_PROF_LOG")) {
            static int period = std::max(1, atoi(pl));
            static int frame  = 0;
            if (++frame % period == 0) {
                const auto& nodes = Haruka::Profiler::get().nodes();
                fprintf(stderr, "--- profiler frame %d ---\n", frame);
                for (size_t i = 1; i < nodes.size(); ++i)
                    fprintf(stderr, "%*s%-24s %7.2f ms  x%d\n", (nodes[i].depth - 1) * 2, "",
                            nodes[i].name.c_str(), nodes[i].ms, nodes[i].count);
            }
        }

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

        // DEBUG TEMPORAL: HARUKA_SHOT=<ruta.ppm> vuelca el framebuffer tras N frames (HARUKA_SHOT_FRAME,
        // por defecto 900) y sale. Para inspeccionar lo que se renderiza sin capturar la pantalla.
        if (const char* shot = getenv("HARUKA_SHOT")) {
            static bool taken = false;
            const int secs = getenv("HARUKA_SHOT_SEC") ? atoi(getenv("HARUKA_SHOT_SEC")) : 70;
            if (!taken && SDL_GetTicks() > (uint64_t)secs * 1000) {
                taken = true;
                int w = (int)_window->getWidth(), h = (int)_window->getHeight();
                std::vector<unsigned char> px((size_t)w * h * 3);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glReadBuffer(GL_BACK);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
                if (FILE* f = fopen(shot, "wb")) {
                    fprintf(f, "P6\n%d %d\n255\n", w, h);
                    for (int y = h - 1; y >= 0; --y) fwrite(&px[(size_t)y * w * 3], 1, (size_t)w * 3, f); // GL: origen abajo
                    fclose(f);
                    fprintf(stderr, "[SHOT] escrito %s (%dx%d)\n", shot, w, h);
                }
                running = false;
            }
        }

        _window->swapBuffers();

        // Frame-rate cap (battery/heat on laptops; 0 = uncapped). With vsync on,
        // swapBuffers already blocks to refresh — this only caps below that.
        int maxFps = Haruka::SettingsManager::get().graphics().maxFps;
        if (maxFps > 0) {
            const double targetNs = 1.0e9 / (double)maxFps;
            const double workNs = std::chrono::duration<double, std::nano>(
                std::chrono::high_resolution_clock::now() - _frameStart).count();
            if (workNs < targetNs)
                SDL_DelayNS((Uint64)(targetNs - workNs));
        }

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


}} // namespace Haruka::Core
