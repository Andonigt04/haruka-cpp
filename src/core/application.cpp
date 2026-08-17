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
#include <cstdlib>   // setenv/getenv — offload PRIME antes de crear el contexto GL
#include <cmath>
#include <csignal>
#include <atomic>
#include <filesystem>
#include <cstring>

#include "core/logger.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_vulkan.h>

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
        if (auto* c = dev->beginFrame())        // beginFrame() puede devolver null (VK: sin swapchain/imagen aún)
            c->setViewport(0, 0, (int)width, (int)height);

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
    _instancing.reset();
    _cascadedShadow.reset();

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

    // ImGui window + settings ANTES de crear la ventana y el device: el backend RHI se elige por el
    // setting persistente RenderBackend (settings → imgui.ini). Hace falta conocer el backend ANTES
    // de crear la ventana para elegir su flag: GL (SDL_WINDOW_OPENGL) o Vulkan (SDL_WINDOW_VULKAN).
    // SettingsManager::init lee ese ini vía ImGui, así que el contexto ImGui se crea aquí (el init
    // del RENDERER de ImGui —GL— va tras el device, que es lo que depende del contexto GL real).
    bool useVulkan = false;
    if (!m_headless) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        Haruka::SettingsManager::get().init();   // carga imgui.ini → m_graphics.renderBackend
        useVulkan = Haruka::SettingsManager::get().graphics().renderBackend
                    == Haruka::Settings::RenderBackend::Vulkan;

        // `HARUKA_BACKEND=vulkan|opengl` fuerza el backend para ESTA ejecución sin tocar los
        // ajustes. Existe para poder COMPARAR los dos backends sin editar `imgui.ini` y acordarse
        // de deshacerlo.
        //
        // ⚠️ SE APLICA AQUÍ, ANTES DE CREAR LA VENTANA, y esa posición es obligatoria: el tipo de
        // ventana (`SDL_WINDOW_VULKAN` vs `SDL_WINDOW_OPENGL`) sale de `useVulkan`. Aplicándolo más
        // abajo —solo al device— la ventana salía de un backend y el device de otro, el contexto no
        // se podía crear, `RHI::device()` devolvía null y el primer `createBuffer` del frame
        // segfallaba. Medido: con el ajuste en Vulkan y `HARUKA_BACKEND=opengl`, SIGSEGV al arrancar.
        if (const char* be = std::getenv("HARUKA_BACKEND")) {
            if (!std::strcmp(be, "vulkan") || !std::strcmp(be, "vk")) {
                useVulkan = true;
                HARUKA_LOGI("RHI", "HARUKA_BACKEND=vulkan -> backend forzado (el ajuste no se toca)");
            } else if (!std::strcmp(be, "opengl") || !std::strcmp(be, "gl")) {
                useVulkan = false;
                HARUKA_LOGI("RHI", "HARUKA_BACKEND=opengl -> backend forzado (el ajuste no se toca)");
            } else {
                HARUKA_LOGW("RHI", "HARUKA_BACKEND='%s' no reconocido (usa 'vulkan' u 'opengl')", be);
            }
        }

        // ── ELEGIR GPU TAMBIÉN EN OPENGL: offload PRIME ─────────────────────────────────────────
        //
        // OpenGL no permite elegir adaptador desde la API, pero SÍ desde el entorno — y tiene que
        // estar puesto ANTES de que se cree el contexto, porque el driver lo lee al inicializarse.
        // Este hueco (ajustes ya cargados, ventana todavía no) es el único sitio donde cabe.
        //
        // Sin esto, en un portátil híbrido el juego corre siempre en la integrada y el desplegable
        // de tarjeta gráfica no puede hacer nada: se elige la dedicada y no pasa nada.
        //
        // Solo se toca el entorno si el usuario eligió explícitamente: sin elección no se fuerza
        // nada y manda la configuración del sistema, que es lo que espera quien no lo ha tocado.
        // Y NO se pisa una variable que ya venga puesta desde fuera — quien arranca con
        // `__NV_PRIME_RENDER_OFFLOAD=1` a mano está diciendo algo más específico que el ajuste.
        const auto& gpus = Haruka::SettingsManager::get().graphics().preferredGpus;
        if (!useVulkan && !gpus.empty() && !gpus[0].empty()) {
            std::string want = gpus[0];
            std::transform(want.begin(), want.end(), want.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            const bool isNvidia = want.find("nvidia")  != std::string::npos
                               || want.find("geforce") != std::string::npos
                               || want.find("rtx")     != std::string::npos
                               || want.find("gtx")     != std::string::npos;
            if (isNvidia) {
                // Offload PRIME de NVIDIA, tal como lo documenta el propio driver.
                if (!std::getenv("__NV_PRIME_RENDER_OFFLOAD"))
                    setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 0);
                if (!std::getenv("__GLX_VENDOR_LIBRARY_NAME"))
                    setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 0);
                HARUKA_LOGI("RHI", "GPU '%s': activado offload PRIME de NVIDIA para el contexto GL",
                            gpus[0].c_str());
            } else if (!std::getenv("DRI_PRIME")) {
                // Mesa (AMD/Intel): DRI_PRIME=1 pide la NO predeterminada. Es lo único que Mesa
                // ofrece sin conocer el id PCI, así que solo se pone si NO se pidió una NVIDIA.
                setenv("DRI_PRIME", "1", 0);
                HARUKA_LOGI("RHI", "GPU '%s': DRI_PRIME=1 para el contexto GL", gpus[0].c_str());
            }
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
    } else if (!_window->init(useVulkan)) {
        HARUKA_MOTOR_ERROR(ErrorCode::WINDOW_CREATION_FAILED, "Failed to initialize Window system.");
        return;
    }

    // RHI: crea el device sobre la ventana. Con backend GL la Window/GLDevice crea el contexto GL
    // correspondiente; con Vulkan la ventana es Vulkan-only (SDL_WINDOW_VULKAN) y el VKDevice crea
    // surface/swapchain. Lo publicamos como device global para que los wrappers (Texture, …) lo usen.
    //
    // El backend se elige por el setting ya cargado arriba. Cambiarlo REQUIERE REINICIAR (el
    // device se crea aquí, una vez). Vulkan cae a OpenGL si no está o si falla al inicializar.
    const auto& gfx = Haruka::SettingsManager::get().graphics();
    // Sigue a `useVulkan` (ya con el override aplicado), NO al ajuste: la ventana se creó con ese
    // criterio y device y ventana tienen que ser del mismo backend.
    const Haruka::RHI::Backend requestedBackend =
        useVulkan ? Haruka::RHI::Backend::Vulkan : Haruka::RHI::Backend::OpenGL;

    // La GPU preferida sale del mismo ajuste, y viaja como LISTA de nombres (ver
    // `GraphicsSettings::preferredGpus`). Vacía = automática. En OpenGL se ignora: la API no permite
    // elegir adaptador, y el panel de ajustes lo dice en vez de fingir que sí.
    _device = Haruka::RHI::Device::create(requestedBackend, _window->getNativeWindow(),
                                          gfx.preferredGpus);
    if (!gfx.preferredGpus.empty() && !gfx.preferredGpus[0].empty())
        HARUKA_LOGI("RHI", "GPU preferida por ajuste: '%s'%s", gfx.preferredGpus[0].c_str(),
                    requestedBackend == Haruka::RHI::Backend::OpenGL
                        ? " (IGNORADA: OpenGL no permite elegir adaptador)" : "");
    Haruka::RHI::setDevice(_device.get());

    // ── QUÉ GPUs VE EL SISTEMA ──────────────────────────────────────────────────────────────────
    //
    // Se lista al arrancar y no solo al abrir el panel: "mi tarjeta no sale en la lista" es una
    // pregunta que se contesta con esta línea en el log, sin tener que reproducir nada ni abrir la
    // UI. Marca cuál es dedicada y cuál coincide con lo elegido.
    if (!m_headless) {
        const auto adapters = Haruka::RHI::Device::enumerateAdapters(
            requestedBackend, _window->getNativeWindow());
        for (size_t i = 0; i < adapters.size(); ++i)
            HARUKA_LOGI("RHI", "  GPU detectada [%zu] %s%s", i, adapters[i].name.c_str(),
                        adapters[i].discrete ? "  [dedicada]" : "");
        if (adapters.size() <= 1)
            HARUKA_LOGI("RHI", "  (solo una GPU listada: sin Vulkan disponible no se pueden "
                               "enumerar adaptadores y solo se ve la que ya usa el contexto)");
    }

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

    // ImGui — renderer GPU. OpenGL → impl GL (necesita un contexto GL real). Vulkan → ImGui_ImplVulkan
    // (fase 7): el device lo inicia (render pass/framebuffers propios + backend de ImGui) y la UI se
    // graba en el command buffer del frame ANTES de presentar (ver VKDevice::endFrame/drawImgui).
    if (!m_headless && _device && _device->backend() == Haruka::RHI::Backend::OpenGL) {
        ImGui_ImplSDL3_InitForOpenGL(_window->getNativeWindow(), SDL_GL_GetCurrentContext());
        ImGui_ImplOpenGL3_Init("#version 450");
        m_imguiGL = true;
    } else if (!m_headless && _device && _device->backend() == Haruka::RHI::Backend::Vulkan) {
        ImGui_ImplSDL3_InitForVulkan(_window->getNativeWindow());
        m_imguiVK = _device->initUi();
        if (!m_imguiVK)
            HARUKA_LOGW("RHI", "initUi() Vulkan falló — la UI no se dibujará.");
    } else {
        HARUKA_LOGW("RHI", "ImGui sin renderer backend inicializado para este modo.");
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

    // `HARUKA_SHOT_AFTER=<segundos>[,ruta.png]`: espera a que el mundo esté cargado, captura un
    // frame limpio (sin HUD) y SALE. Existe para poder COMPARAR backends: el arranque en frío ronda
    // el minuto y el backend solo se elige al inicio, así que sin esto "¿se ve igual en Vulkan que
    // en OpenGL?" no es una pregunta que se pueda contestar con una medida — solo de memoria.
    // Junto con `HARUKA_BACKEND` da dos PNG comparables del mismo escenario.
    double shotAfterS = -1.0;
    std::string shotPath;
    if (const char* sa = std::getenv("HARUKA_SHOT_AFTER")) {
        const std::string s(sa);
        const size_t comma = s.find(',');
        shotAfterS = std::atof(s.substr(0, comma).c_str());
        if (comma != std::string::npos) shotPath = s.substr(comma + 1);
        HARUKA_LOGI("Shot", "HARUKA_SHOT_AFTER=%.1f s -> captura y salida%s%s",
                    shotAfterS, shotPath.empty() ? "" : " -> ", shotPath.c_str());
    }
    const auto shotT0 = std::chrono::steady_clock::now();
    bool shotRequested = false, shotDone = false;

    bool running = true;
    while (running) {
        if (g_sigintReceived.load()) { running = false; continue; }

        if (shotAfterS > 0.0) {
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - shotT0).count();
            // Dos fases: primero se PIDE la captura, y solo se sale en la vuelta SIGUIENTE — la
            // captura ocurre dentro del render del frame, así que salir en la misma iteración
            // dejaría el PNG a medias o sin escribir.
            if (!shotRequested && elapsed >= shotAfterS) {
                requestScreenshot(shotPath);
                shotRequested = true;
            } else if (shotRequested && !shotDone) {
                shotDone = true;
            } else if (shotDone) {
                HARUKA_LOGI("Shot", "captura hecha, saliendo");
                running = false;
                continue;
            }
        }

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
                if (!consumed && (m_imguiGL || m_imguiVK))
                    ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT) running = false;
                if (event.type == SDL_EVENT_WINDOW_RESIZED) {
                    // ⚠️ EN PÍXELES, NO EN UNIDADES LÓGICAS. `event.window.data1/2` traen el tamaño
                    // LÓGICO, y con escalado del compositor (Wayland fraccional, HiDPI) no coincide
                    // con el del framebuffer. El swapchain de Vulkan se crea con el tamaño en
                    // PÍXELES, así que el motor acababa creyendo 1920x1080 con un swapchain de
                    // 1280x720: `renderArea` mayor que el framebuffer (inválido) y capturas
                    // repetidas en horizontal. Medido en una captura de RenderDoc.
                    int pw = event.window.data1, ph = event.window.data2;
                    SDL_GetWindowSizeInPixels(_window->getNativeWindow(), &pw, &ph);
                    if (pw <= 0 || ph <= 0) { pw = event.window.data1; ph = event.window.data2; }
                    _window->setWidth((uint32_t)pw);
                    _window->setHeight((uint32_t)ph);
                    m_editorViewportW = pw;
                    m_editorViewportH = ph;
                    if (RHI::Device* dev = RHI::device())
                        if (auto* c = dev->beginFrame())
                            c->setViewport(0, 0, pw, ph);
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

        // Start ImGui frame. ImGui::NewFrame() define el SCOPE del frame (el juego llama ImGui::Begin en
        // sus menús: sin NewFrame aborta). Los impl*_NewFrame y el dibujo existen según backend:
        // GL → impl GL; Vulkan → ImGui_ImplVulkan (la escena 3D es RHI y corre igual).
        if (!m_headless) {
            if (m_imguiGL) {
                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplSDL3_NewFrame();
            } else if (m_imguiVK) {
                ImGui_ImplSDL3_NewFrame();               // fija io.DisplaySize (lógico) para el ratón
                // Escala real = extent físico del swapchain / DisplaySize lógico. Es el único par que
                // usa ImGui_ImplVulkan (viewport = DisplaySize * FramebufferScale) → con esto la UI
                // cubre EXACTO el framebuffer. Se recalcula por-frame (resize / cambio de monitor).
                {
                    ImGuiIO& io = ImGui::GetIO();
                    uint32_t fbw = 0, fbh = 0;
                    if (_device) _device->framebufferSize(fbw, fbh);
                    if (fbw && fbh && io.DisplaySize.x > 1.f && io.DisplaySize.y > 1.f) {
                        const float sx = (float)fbw / io.DisplaySize.x;
                        const float sy = (float)fbh / io.DisplaySize.y;
                        io.DisplayFramebufferScale = ImVec2(sx, sy);
                        // Re-rasterizamos el atlas de fuentes a la resolución física (fuente escalada +
                        // FontGlobalScale compensado) para que el texto salga NÍTIDO y del tamaño lógico
                        // esperado. Solo la primera vez (o si cambia de forma significativa).
                        const float sc = std::max(sx, sy);
                        if (std::fabs(sc - m_imguiFbScale) > 0.05f) {
                            m_imguiFbScale = sc;
                            ImFontConfig cfg;
                            cfg.SizePixels = 13.0f * sc;
                            cfg.OversampleH = 3; cfg.OversampleV = 3;
                            io.Fonts->Clear();
                            io.Fonts->AddFontDefault(&cfg);
                            io.FontGlobalScale = 1.0f / sc;
                        }
                    }
                }
                ImGui_ImplVulkan_NewFrame();   // construye el atlas (ya a la resolución correcta)
            } else {
                ImGuiIO& io = ImGui::GetIO();
                io.DisplaySize = ImVec2((float)_window->getWidth(), (float)_window->getHeight());
                io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
                // Sin backend de render (backend de respaldo/editor): construimos el atlas CPU-side con
                // un texID ficticio para que NewFrame() no aborte; la UI no se pinta.
                if (ImFontAtlas* atlas = io.Fonts) {
                    if (!atlas->IsBuilt()) {
                        atlas->Build();
                        atlas->SetTexID((ImTextureID)(intptr_t)1);
                    }
                }
            }
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

        // ImGui composite — Render() siempre (cierra el scope del frame del menú del juego); el DIBUJO
        // (RenderDrawData) solo existe con impl GL activo (Vulkan: fase 7 → ImGui_ImplVulkan).
        if (!m_headless) {
            ImGui::Render();
            if (m_imguiGL)
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            else if (m_imguiVK) {
                // Fase 7: adjuntamos el draw data de la UI al context activo del frame. El device lo
                // graba y presenta en endFrame() (drawImgui); beginFrame() es idempotente (mismo cmd).
                if (RHI::Context* c = _device->beginFrame())
                    c->setUiDrawData(ImGui::GetDrawData());
            }
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

        // ⚠️ MEDIR EL SWAP no es cosmético: es donde se hace visible el coste de la GPU. La CPU solo
        // ENCOLA los draws (por eso `simple_planet.draw` marca <0,5 ms aunque el terreno cueste
        // decenas de ms de GPU); el bloqueo real aparece aquí, cuando el driver espera a que la GPU
        // termine o a que llegue el vsync. Sin este scope, un frame limitado por GPU parece tiempo
        // "perdido" dentro del render y se acaba optimizando el sitio equivocado.
        if (RHI::device()) {
            HARUKA_PROFILE("present.swap(espera GPU/vsync)");
            RHI::device()->endFrame();
        }

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

    if (m_imguiGL) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
    } else if (m_imguiVK) {
        // El renderer de ImGui_ImplVulkan lo apaga el propio device en su destructor (necesita la
        // cola/device; corre tras este run() al destruirse _device). Aquí solo la plataforma SDL3.
        ImGui_ImplSDL3_Shutdown();
    }
    if (!m_headless && !m_imguiVK) {
        // Con Vulkan el context se destruye al final del proceso (tras el dtormr del device), para que
        // ImGui_ImplVulkan_Shutdown (device dtor) corra con el context aún vivo.
        ImGui::DestroyContext();
    }
}


}} // namespace Haruka::Core
