// Application — lifecycle & orchestration.
// Construction/teardown, scene loading, planetary-system bring-up, graphics
// settings application, FBO (re)creation and the standalone main loop. The
// per-frame rendering lives in application_render.cpp, the GL asset caches in
// application_assets.cpp and the DGS network bridge in application_network.cpp.

#include "application.h"
#include "application_internal.h"

#include "rhi/rhi_context.h"

#include <iostream>
#include <algorithm>
#include <csignal>
#include <atomic>
#include <filesystem>
#include <cstring>

#include "core/logger.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>

#include "renderer/motor_instance.h"
#include "renderer/shader.h"
#include "game/planetary_system.h"
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

    // SISTEMA PLANETARIO. Estaba solo en loadScene(), así que quien entra por init() —el EDITOR, que
    // trae su propia SceneManager— se quedaba con `_planetarySystem` nulo. Y de él cuelga casi todo
    // lo que se ve: el pase de CIELO, el terreno y los SimplePlanet están todos dentro de
    // `if (_planetarySystem)`. Con una escena que tuviera un planeta, el viewport del IDE no
    // mostraba NADA y no había error que lo dijera. El IDE no puede llamarlo él: no sabe qué es un
    // planeta a propósito — la interpretación escena→cuerpos celestes es del motor.
    initPlanetarySystem();

    MotorInstance::getInstance().setApplication(this);
    MotorInstance::getInstance().setScene(_currentScene);
    MotorInstance::getInstance().setCamera(_camera.get());
}

void Application::recreateFBOs(int newWidth, int newHeight) {
    _window->setWidth(newWidth);
    _window->setHeight(newHeight);

    uint32_t width = _window->getWidth();
    uint32_t height = _window->getHeight();

    // 1. Update the viewport via RHI
    if (RHI::Device* dev = RHI::device())
        dev->beginFrame()->setViewport(0, 0, (int)width, (int)height);

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
    _planetarySystem.reset();
    _worldSystem.reset();
    _raycastSystem.reset();

    if (RHI::Device* dev = RHI::device()) {
        if (RHI::valid(m_uboPerFrameH))  { dev->destroy(m_uboPerFrameH);  m_uboPerFrameH  = {}; }
        if (RHI::valid(m_uboPerObjectH)) { dev->destroy(m_uboPerObjectH); m_uboPerObjectH = {}; }
        if (RHI::valid(m_quadBuf))       { dev->destroy(m_quadBuf);       m_quadBuf       = {}; }
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
    m_bloomTexH[0] = m_bloomTexH[1] = {};
    m_bloomW = m_bloomH = 0;
    _postScene.reset();

    // Render-pipeline objects with GL resources in their destructors
    _shadow.reset();
    _hdr.reset();
    _bloom.reset();
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

void Application::run(const std::string& startScenePath, bool headless) {
    m_headless = headless;
    uint32_t _width = 1280;
    uint32_t _height = 720;

    // Raíz de assets para JUEGOS STANDALONE. El IDE la fija explícitamente (Shader::setBaseDir
    // en editor_app.cpp) porque NO usa Application::run; un juego que SÍ pasa por aquí la derivaba
    // del cwd y, si nadie la fijaba, `shaderIncludeDir()` quedaba VACÍA → los #include de los
    // shaders del planeta ("lib/terrain_material.glsl") no se resolvían y el pipeline fallaba.
    // Guard: solo si nadie la fijó ya (el juego puede querer su propia raíz ANTES de run()).
    if (Haruka::Renderer::Shader::baseDir().empty()) {
        std::error_code ec;
        std::filesystem::path exeDir = std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
        if (!ec && !exeDir.empty()) {
            Haruka::Renderer::Shader::setBaseDir((exeDir / "assets/").string().c_str());
        } else {
            Haruka::Renderer::Shader::setBaseDir("assets/");
        }
    }

    _window = std::make_unique<Haruka::Core::Window>(
        Haruka::Core::WindowProps("Survival", _width, _height, "assets/icons/icon.png")
    );
    if (m_headless) {
        if (!_window->initHeadless()) {
            HARUKA_MOTOR_ERROR(ErrorCode::WINDOW_CREATION_FAILED, "Failed to initialize headless Window system.");
            return;
        }
    } else if (!_window->init()) {
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
            HARUKA_LOGI("RHI", "Backend activo: %s (solicitado, sin fallback).", beName(_device->backend()));
        else
            HARUKA_LOGW("RHI", "Backend activo: %s (FALLBACK desde %s).",
                        beName(_device->backend()), beName(requestedBackend));
    } else {
        HARUKA_LOGW("RHI", "No hay device — el motor correrá por las rutas GL directas de compatibilidad.");
    }

    // ImGui — standalone runtime owns the context (skipped in headless mode)
    if (!m_headless) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGui_ImplSDL3_InitForOpenGL(_window->getNativeWindow(), SDL_GL_GetCurrentContext());
        ImGui_ImplOpenGL3_Init("#version 450");
    }

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

        // Headless: no window events, just yield
        if (m_headless) {
            SDL_DelayNS(16666666); // ~60 fps pacing
        } else {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                bool consumed = false;
                if (_gameInterface && _gameInterface->onEvent)
                    consumed = _gameInterface->onEvent(&event);
                if (!consumed)
                    ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT) running = false;
                if (event.type == SDL_EVENT_WINDOW_RESIZED) {
                    _window->setWidth(event.window.data1);
                    _window->setHeight(event.window.data2);
                    m_editorViewportW = event.window.data1;
                    m_editorViewportH = event.window.data2;
                    if (RHI::Device* dev = RHI::device())
                        dev->beginFrame()->setViewport(0, 0, event.window.data1, event.window.data2);
                }
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
                HARUKA_LOGD("Profiler", "--- frame %d ---", frame);
                for (size_t i = 1; i < nodes.size(); ++i)
                    HARUKA_LOGD("Profiler", "%*s%-24s %7.2f ms  x%d", (nodes[i].depth - 1) * 2, "",
                            nodes[i].name.c_str(), nodes[i].ms, nodes[i].count);
            }
        }

        // Start ImGui frame (skipped in headless mode)
        if (!m_headless) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
        }

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

        // ImGui composite — draw all ImGui widgets over the 3D scene (skipped in headless)
        if (!m_headless) {
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }


        // DEBUG TEMPORAL: HARUKA_SHOT=<ruta.ppm> vuelca el framebuffer tras N segundos (HARUKA_SHOT_SEC,
        // por defecto 900) y sale. Para inspeccionar lo que se renderiza sin capturar la pantalla.
        if (const char* shot = getenv("HARUKA_SHOT")) {
            static bool taken = false;
            const int secs = getenv("HARUKA_SHOT_SEC") ? atoi(getenv("HARUKA_SHOT_SEC")) : 70;
            if (!taken && SDL_GetTicks() > (uint64_t)secs * 1000) {
                taken = true;
                int w = (int)_window->getWidth(), h = (int)_window->getHeight();
                std::vector<unsigned char> px((size_t)w * h * 4);
                if (RHI::Device* dev = RHI::device()) {
                    dev->readPixels(0, 0, w, h, RHI::Format::RGBA8, px.data());
                }
                if (FILE* f = fopen(shot, "wb")) {
                    fprintf(f, "P6\n%d %d\n255\n", w, h);
                    // Convert RGBA → RGB for PPM output
                    std::vector<unsigned char> rgb((size_t)w * h * 3);
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x) {
                            size_t src = (size_t)y * w * 4 + (size_t)x * 4;
                            size_t dst = (size_t)y * w * 3 + (size_t)x * 3;
                            rgb[dst] = px[src]; rgb[dst+1] = px[src+1]; rgb[dst+2] = px[src+2];
                        }
                    for (int y = h - 1; y >= 0; --y)
                        fwrite(&rgb[(size_t)y * w * 3], 1, (size_t)w * 3, f);
                    fclose(f);
                    HARUKA_LOGI("Shot", "escrito %s (%dx%d)", shot, w, h);
                }
                running = false;
            }
        }

        if (RHI::device())
            RHI::device()->endFrame();

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

    if (!m_headless) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
}


}} // namespace Haruka::Core
