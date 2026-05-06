#include "application.h"

#include <iostream>

#include <SDL3/SDL.h>

#include "renderer/motor_instance.h"

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

void Application::recreateFBOs(int newWidth, int newHeight) {
    _width = std::max(1, newWidth);
    _height = std::max(1, newHeight);
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

void Application::create_window() {
    if (_window) return;

    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "[Application] Failed to initialize SDL video" << std::endl;
        return;
    }

    _window = SDL_CreateWindow("Haruka Runtime", _width, _height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!_window) {
        std::cerr << "[Application] Failed to create window: " << SDL_GetError() << std::endl;
    }
}

void Application::create_gl_context() {
    if (!_window || _glContext) return;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);

    _glContext = SDL_GL_CreateContext(_window);
    if (!_glContext) {
        std::cerr << "[Application] Failed to create GL context: " << SDL_GetError() << std::endl;
        return;
    }

    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        std::cerr << "[Application] Failed to initialize GLAD" << std::endl;
        return;
    }

    glViewport(0, 0, _width, _height);
    glEnable(GL_DEPTH_TEST);
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
    glViewport(0, 0, _width, _height);
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

    if (_window && _glContext) {
        SDL_GL_SwapWindow(_window);
    }

    _fpsFrameCount++;
    _fpsLastTime += deltaTime;
    if (_fpsLastTime >= 1.0) {
        _lastFps = static_cast<float>(_fpsFrameCount / _fpsLastTime);
        _fpsFrameCount = 0;
        _fpsLastTime = 0.0;
    }
}

void Application::main_loop() {
    bool running = true;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_WINDOW_RESIZED) {
                recreateFBOs(event.window.data1, event.window.data2);
            }
        }

        renderFrame();
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

    if (_glContext) {
        SDL_GL_DestroyContext(_glContext);
        _glContext = nullptr;
    }
    if (_window) {
        SDL_DestroyWindow(_window);
        _window = nullptr;
    }

    if (SDL_WasInit(SDL_INIT_VIDEO)) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
}

void Application::run(const std::string& startScenePath) {
    create_window();
    create_gl_context();

    loadScene(startScenePath);
    if (!_currentScene) {
        _ownedScene = std::make_unique<Haruka::SceneManager>();
        _currentScene = _ownedScene.get();
    }

    init(*_currentScene);
    main_loop();
}
