#include "application.h"

#include <iostream>
#include <unordered_map>

#include <SDL3/SDL.h>

#include <glm/gtx/euler_angles.hpp>

#include "renderer/motor_instance.h"
#include "core/components/mesh_renderer_component.h"
#include "renderer/model.h"
#include "renderer/primitive_shapes.h"
#include "core/scene/scene_render_policy.h"
#include "tools/error_reporter.h"

namespace {
std::vector<Haruka::RenderCommand> g_sceneRenderQueue;

glm::mat4 getTransformMatrix(const Haruka::SceneObject& obj) {
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(obj.position));

    glm::mat4 rotation = glm::eulerAngleXYZ(
        glm::radians(static_cast<float>(obj.rotation.x)),
        glm::radians(static_cast<float>(obj.rotation.y)),
        glm::radians(static_cast<float>(obj.rotation.z))
    );

    transform *= rotation;
    return glm::scale(transform, glm::vec3(obj.scale));
}

Model* getOrLoadModelCached(const std::string& path) {
    static std::unordered_map<std::string, std::shared_ptr<Model>> modelCache;

    auto it = modelCache.find(path);
    if (it != modelCache.end()) {
        return it->second.get();
    }

    auto model = std::make_shared<Model>(path);
    Model* modelPtr = model.get();
    modelCache.emplace(path, std::move(model));
    return modelPtr;
}

SimpleMesh* getPrimitiveMesh(Haruka::PrimitiveType primitive) {
    static std::unique_ptr<SimpleMesh> sphereMesh;
    static std::unique_ptr<SimpleMesh> cubeMesh;
    static std::unique_ptr<SimpleMesh> capsuleMesh;
    static std::unique_ptr<SimpleMesh> planeMesh;

    switch (primitive) {
        case Haruka::PrimitiveType::CUBE:
            if (!cubeMesh) {
                std::vector<glm::vec3> vertices;
                std::vector<glm::vec3> normals;
                std::vector<unsigned int> indices;
                PrimitiveShapes::createCube(1.0f, vertices, normals, indices);
                cubeMesh = std::make_unique<SimpleMesh>(vertices, normals, indices);
            }
            return cubeMesh.get();
        case Haruka::PrimitiveType::SPHERE:
            if (!sphereMesh) {
                std::vector<glm::vec3> vertices;
                std::vector<glm::vec3> normals;
                std::vector<unsigned int> indices;
                PrimitiveShapes::createSphereLOD(1.0f, 24, 16, vertices, normals, indices);
                sphereMesh = std::make_unique<SimpleMesh>(vertices, normals, indices);
            }
            return sphereMesh.get();
        case Haruka::PrimitiveType::CAPSULE:
            if (!capsuleMesh) {
                std::vector<glm::vec3> vertices;
                std::vector<glm::vec3> normals;
                std::vector<unsigned int> indices;
                PrimitiveShapes::createCapsule(0.5f, 1.5f, 24, 12, vertices, normals, indices);
                capsuleMesh = std::make_unique<SimpleMesh>(vertices, normals, indices);
            }
            return capsuleMesh.get();
        case Haruka::PrimitiveType::PLANE:
            if (!planeMesh) {
                std::vector<glm::vec3> vertices;
                std::vector<glm::vec3> normals;
                std::vector<unsigned int> indices;
                PrimitiveShapes::createPlane(1.0f, 1.0f, 1, vertices, normals, indices);
                planeMesh = std::make_unique<SimpleMesh>(vertices, normals, indices);
            }
            return planeMesh.get();
        default:
            return nullptr;
    }
}
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

    g_sceneRenderQueue = buildSceneRenderQueue(*_currentScene);

    _iTotalDrawCalls = static_cast<int>(g_sceneRenderQueue.size());
    _iRenderedDrawCalls = _iTotalDrawCalls;
}

void Application::renderFrameContent() {
    const uint32_t width = _window ? _window->getWidth() : 0u;
    const uint32_t height = _window ? _window->getHeight() : 0u;

    if (_editorTarget) {
        _editorTarget->bindForWriting();
    }

    glViewport(0, 0, width, height);
    glClearColor(0.01f, 0.01f, 0.01f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    _iTotalDrawCalls = static_cast<int>(g_sceneRenderQueue.size());
    _iRenderedDrawCalls = 0;
    _iTotalVertices = 0;
    _iTotalTriangles = 0;
    _iRenderedVertices = 0;
    _iRenderedTriangles = 0;

    if (_currentScene && _camera) {
        const bool useFinalLook = getRenderFeatureHDR() || getRenderFeatureBloom() || getRenderFeatureSSAO() || getRenderFeatureIBL() || getRenderFeatureShadows();
        if (!_mainShader || _mainShaderUsesFinalLook != useFinalLook) {
            _mainShader = std::make_unique<Shader>(
                "shaders/simple.vert",
                useFinalLook ? "shaders/final.frag" : "shaders/preview.frag"
            );
            _mainShaderUsesFinalLook = useFinalLook;
        }

        _mainShader->use();

        const float aspect = (height > 0u) ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
        const glm::vec3 cameraOrigin = glm::vec3(_camera->position);
        const glm::mat4 viewNoTranslation = glm::mat4(glm::mat3(_camera->getViewMatrix()));
        _mainShader->setMat4(1, viewNoTranslation);
        _mainShader->setMat4(2, _camera->getProjectionMatrix(aspect));

        if (useFinalLook) {
            _mainShader->setVec3("cameraPos", cameraOrigin);
            _mainShader->setVec3("sunDirection", glm::normalize(glm::vec3(0.35f, 0.75f, 0.25f)));
            _mainShader->setBool("enableHDR", getRenderFeatureHDR());
            _mainShader->setBool("enableBloom", getRenderFeatureBloom());
            _mainShader->setBool("enableSSAO", getRenderFeatureSSAO());
            _mainShader->setBool("enableIBL", getRenderFeatureIBL());
            _mainShader->setBool("enableShadows", getRenderFeatureShadows());
        }

        int renderedDrawCalls = 0;
        int renderedVertices = 0;
        int renderedTriangles = 0;

        for (const auto& command : g_sceneRenderQueue) {
            const auto* obj = command.object;
            if (!obj) continue;

            const glm::mat4 modelMatrix = glm::translate(glm::mat4(1.0f), -cameraOrigin) * getTransformMatrix(*obj);
            _mainShader->setMat4(0, modelMatrix);

            if (useFinalLook) {
                glm::vec3 baseColor = glm::vec3(obj->color);
                if (glm::length(baseColor) < 0.001f) {
                    baseColor = glm::vec3(0.75f, 0.76f, 0.80f);
                }
                _mainShader->setVec3("baseColor", baseColor);
            }

            switch (command.kind) {
                case Haruka::RenderKind::Model: {
                    Model* model = getOrLoadModelCached(obj->modelPath);
                    if (!model) break;
                    model->Draw(*_mainShader);
                    ++renderedDrawCalls;
                    renderedVertices += model->getVertexCount();
                    renderedTriangles += model->getTriangleCount();
                    break;
                }
                case Haruka::RenderKind::MeshComponent: {
                    if (!obj->meshRenderer || !obj->meshRenderer->isResident()) break;
                    obj->meshRenderer->render(*_mainShader);
                    ++renderedDrawCalls;
                    renderedVertices += obj->meshRenderer->getResidentVertexCount();
                    renderedTriangles += obj->meshRenderer->getResidentTriangleCount();
                    break;
                }
                case Haruka::RenderKind::Primitive: {
                    SimpleMesh* primitiveMesh = getPrimitiveMesh(command.primitive);
                    if (!primitiveMesh) break;
                    primitiveMesh->draw();
                    ++renderedDrawCalls;
                    renderedVertices += primitiveMesh->getVertexCount();
                    renderedTriangles += primitiveMesh->getTriangleCount();
                    break;
                }
                case Haruka::RenderKind::None:
                    break;
            }
        }

        _iRenderedDrawCalls = renderedDrawCalls;
        _iRenderedVertices = renderedVertices;
        _iRenderedTriangles = renderedTriangles;
        _iTotalVertices = renderedVertices;
        _iTotalTriangles = renderedTriangles;
    }

    if (_imguiCallback) {
        _imguiCallback();
    }

    if (_editorTarget) {
        _editorTarget->unbind();
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
    }
}