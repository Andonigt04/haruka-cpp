#include "application.h"

#include <iostream>

#include <SDL3/SDL.h>

#include "renderer/motor_instance.h"
#include "tools/error_reporter.h"

namespace {
std::vector<const Haruka::SceneObject*> g_sceneRenderQueue;
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

void Application::loadScene(const std::string& scenePath) {
    _ownedScene = std::make_unique<Haruka::SceneManager>();
    Haruka::SceneLoader loader(*_ownedScene);

    if (!scenePath.empty() && loader.loadFromFile(scenePath)) {
        _currentScene = _ownedScene.get();
        std::cout << "[Application] Scene loaded: " << scenePath << std::endl;
        return;
    }

    _currentScene = _ownedScene.get();
    std::cout << "[Application] Using empty scene" << std::endl;
}

void Application::init(Haruka::SceneManager& scene) {
    _currentScene = &scene;

    if (!_camera) {
        _camera = std::make_unique<Camera>(glm::vec3(0.0f, 0.0f, 5.0f));
    }

    if (!_worldSystem) {
        _worldSystem = std::make_unique<Haruka::WorldSystem>();
        _worldSystem->init();
    }

    if (!_physicsEngine) {
        _physicsEngine = std::make_unique<Haruka::PhysicsEngine>();
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
    g_sceneRenderQueue.clear();

    if (!_currentScene) return;

    const auto& objects = _currentScene->getAllObjects();
    g_sceneRenderQueue.reserve(objects.size());

    for (const auto& obj : objects) {
        if (obj) g_sceneRenderQueue.push_back(obj.get());
    }

    _iTotalDrawCalls = static_cast<int>(g_sceneRenderQueue.size());
    _iRenderedDrawCalls = _iTotalDrawCalls;
}

void Application::renderFrameContent() {
    glViewport(0, 0, _window->getWidth(), _window->getHeight());
    glClearColor(0.05f, 0.07f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (_imguiCallback) {
        _imguiCallback();
    }
}

void Application::renderFrame() {
    auto now = std::chrono::high_resolution_clock::now();
    const std::chrono::duration<float> elapsed = now - _frameStart;
    _frameStart = now;

    deltaTime = elapsed.count();
    _lastFrameTimeMs = deltaTime * 1000.0f;

    buildRenderQueue();
    renderFrameContent();

    if (_window) {
        _window->swapBuffers();
    }

    _fpsFrameCount++;
    _fpsLastTime += deltaTime;
    if (_fpsLastTime >= 1.0) {
        _lastFps = static_cast<float>(_fpsFrameCount / _fpsLastTime);
        _fpsFrameCount = 0;
        _fpsLastTime = 0.0;
    }
}

void Application::cleanup() {
    g_sceneRenderQueue.clear();

    MotorInstance::getInstance().clear();

    _currentScene = nullptr;
    _ownedScene.reset();

    _physicsEngine.reset();
    _terrainStreamingSystem.reset();
    _chunkCache.reset();
    _planetarySystem.reset();
    _worldSystem.reset();
    _raycastSystem.reset();

    if (quadVBO != 0) {
        glDeleteBuffers(1, &quadVBO);
        quadVBO = 0;
    }
    if (quadVAO != 0) {
        glDeleteVertexArrays(1, &quadVAO);
        quadVAO = 0;
    }

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

    // 1. Instanciar y configurar la ventana
    _window = std::make_unique<Haruka::Core::Window>(
        Haruka::Core::WindowProps("Haruka Engine", _width, _height)
    );
    
    if (!_window->init()) {
        HARUKA_MOTOR_ERROR(ErrorCode::WINDOW_CREATION_FAILED, "Failed to initialize Window system.");
        return;
    }

    // 2. Inicializar lógica del motor
    loadScene(startScenePath);
    init(*_currentScene);

    bool running = true;
    while (running) {
        // Guardamos dimensiones actuales para detectar cambios después de los eventos
        uint32_t lastWidth = _window->getWidth();
        uint32_t lastHeight = _window->getHeight();

        // 3. Procesar Eventos (Delegado a Window)
        _window->pollEvents(running);

        // 4. Detectar Redimensionado
        if (_window->getWidth() != lastWidth || _window->getHeight() != lastHeight) {
            recreateFBOs(_window->getWidth(), _window->getHeight());
        }

        // 5. Renderizar y Swap
        renderFrame();
        _window->swapBuffers();
    }
}