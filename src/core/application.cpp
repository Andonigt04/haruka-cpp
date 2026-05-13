#include "application.h"

#include <iostream>
#include <unordered_map>
#include <algorithm>

#include <SDL3/SDL.h>

#include <glm/gtx/euler_angles.hpp>

#include "renderer/motor_instance.h"
#include "game/planetary_system.h"
#include "core/components/mesh_renderer_component.h"
#include "renderer/model.h"
#include "renderer/primitive_shapes.h"
#include "core/scene/scene_render_policy.h"
#include "tools/error_reporter.h"

// std140-compatible structs mirroring the UBO declarations in the shaders.
// Any change to the GLSL UBO layout must be reflected here.
struct alignas(16) PerFrameUBOData {
    glm::mat4 view;
    glm::mat4 projection;
    glm::vec3 cameraPos;      float _pad0;
    glm::vec3 sunDirection;   float _pad1;
    glm::vec3 sunLightColor;  float ambientStrength;
    int enableHDR;
    int enableBloom;
    int enableSSAO;
    int enableIBL;
    int enableShadows;
    int _pad3[3];
};
static_assert(sizeof(PerFrameUBOData) == 208, "PerFrameUBOData std140 size mismatch");

struct alignas(16) PerObjectUBOData {
    glm::mat4 model;
    glm::vec4 baseColorAndPlanetRadius; // rgb=color, a=planetRadius
    glm::vec4 planetCenterAndFlag;      // xyz=planetCenter, w=useProceduralTerrain
};
static_assert(sizeof(PerObjectUBOData) == 96, "PerObjectUBOData std140 size mismatch");

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

void Application::setEditorViewportSize(int w, int h) {
    m_editorViewportW = w;
    m_editorViewportH = h;
}

void Application::initPlanetarySystem() {
    if (!_currentScene) return;

    _planetarySystem = std::make_unique<Haruka::PlanetarySystem>();
    _planetarySystem->init();

    for (const auto& objPtr : _currentScene->getAllObjects()) {
        if (!objPtr) continue;
        const auto& obj = *objPtr;

        // Only stream terrain for objects that explicitly define terrainSettings with layers
        if (!obj.terrainSettings || obj.terrainSettings->layers.empty()) continue;

        const auto& ts = *obj.terrainSettings;

        Haruka::PlanetarySystem::Planet planet;
        planet.name     = obj.name;
        planet.position = obj.position;
        planet.radius   = std::max({obj.scale.x, obj.scale.y, obj.scale.z});

        planet.terrainSettings["config"]["chunkSize"] = ts.chunkSize > 0 ? ts.chunkSize : 32;
        planet.terrainSettings["config"]["seed"]      = ts.seed;
        for (const auto& [name, layer] : ts.layers) {
            planet.terrainSettings["config"]["layers"][name]["freq"]     = layer.freq;
            planet.terrainSettings["config"]["layers"][name]["octaves"]  = layer.octaves;
            planet.terrainSettings["config"]["layers"][name]["strength"] = layer.strength;
        }

        _planetarySystem->addPlanet(planet);
    }
}

void Application::init(Haruka::SceneManager& scene) {
    _currentScene = &scene;

    #ifdef HARUKA_NETWORK
        m_dgs.connect("head-server", 42424, "player1", "secret", "api", 8080);
    #endif

    if (!_worldSystem) {
        _worldSystem = std::make_unique<Haruka::WorldSystem>();
        _worldSystem->init();
    }

    if (!_physicsEngine) {
        _physicsEngine = std::make_unique<Haruka::PhysicsEngine>();
    }

    if (!_camera) {
        _camera = std::make_unique<Camera>(glm::vec3(0.0f, 0.0f, 5.0f));
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
    const uint32_t width  = _window ? _window->getWidth()  : static_cast<uint32_t>(m_editorViewportW);
    const uint32_t height = _window ? _window->getHeight() : static_cast<uint32_t>(m_editorViewportH);

    if (_editorTarget) {
        _editorTarget->bindForWriting();
    }

    glViewport(0, 0, width, height);
    glClearColor(0.01f, 0.01f, 0.01f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    _iTotalDrawCalls    = static_cast<int>(g_sceneRenderQueue.size());
    _iRenderedDrawCalls = 0;
    _iTotalVertices     = 0;
    _iTotalTriangles    = 0;
    _iRenderedVertices  = 0;
    _iRenderedTriangles = 0;

    if (_currentScene && _camera) {
        // Lazy-create UBOs
        if (m_uboPerFrame == 0) {
            glGenBuffers(1, &m_uboPerFrame);
            glBindBuffer(GL_UNIFORM_BUFFER, m_uboPerFrame);
            glBufferData(GL_UNIFORM_BUFFER, sizeof(PerFrameUBOData), nullptr, GL_DYNAMIC_DRAW);
            glBindBuffer(GL_UNIFORM_BUFFER, 0);
        }
        if (m_uboPerObject == 0) {
            glGenBuffers(1, &m_uboPerObject);
            glBindBuffer(GL_UNIFORM_BUFFER, m_uboPerObject);
            glBufferData(GL_UNIFORM_BUFFER, sizeof(PerObjectUBOData), nullptr, GL_DYNAMIC_DRAW);
            glBindBuffer(GL_UNIFORM_BUFFER, 0);
        }

        const bool useFinalLook = getRenderFeatureHDR() || getRenderFeatureBloom()
                                || getRenderFeatureSSAO() || getRenderFeatureIBL()
                                || getRenderFeatureShadows();
        if (!_mainShader || _mainShaderUsesFinalLook != useFinalLook) {
            _mainShader = std::make_unique<Shader>(
                "shaders/simple.vert",
                useFinalLook ? "shaders/final.frag" : "shaders/preview.frag"
            );
            _mainShaderUsesFinalLook = useFinalLook;
        }

        _mainShader->use();
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_uboPerFrame);
        glBindBufferBase(GL_UNIFORM_BUFFER, 1, m_uboPerObject);

        // Upload per-frame UBO
        const float      aspect       = (height > 0u) ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
        const glm::vec3  cameraOrigin = glm::vec3(_camera->position);

        PerFrameUBOData frameData{};
        frameData.view            = glm::mat4(glm::mat3(_camera->getViewMatrix()));
        frameData.projection      = _camera->getProjectionMatrix(aspect);
        frameData.cameraPos       = cameraOrigin;
        frameData.sunDirection    = glm::normalize(glm::vec3(0.35f, 0.75f, 0.25f));
        frameData.sunLightColor   = glm::vec3(1.0f, 0.98f, 0.95f);
        frameData.ambientStrength = 0.0f;
        frameData.enableHDR       = (int)getRenderFeatureHDR();
        frameData.enableBloom     = (int)getRenderFeatureBloom();
        frameData.enableSSAO      = (int)getRenderFeatureSSAO();
        frameData.enableIBL       = (int)getRenderFeatureIBL();
        frameData.enableShadows   = (int)getRenderFeatureShadows();

        glBindBuffer(GL_UNIFORM_BUFFER, m_uboPerFrame);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(PerFrameUBOData), &frameData);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);

        int renderedDrawCalls = 0;
        int renderedVertices  = 0;
        int renderedTriangles = 0;

        for (const auto& command : g_sceneRenderQueue) {
            const auto* obj = command.object;
            if (!obj) continue;

            glm::vec3 baseColor = glm::vec3(obj->color);
            if (glm::length(baseColor) < 0.001f) baseColor = glm::vec3(0.75f, 0.76f, 0.80f);

            PerObjectUBOData objData{};
            objData.model                    = glm::translate(glm::mat4(1.0f), -cameraOrigin) * getTransformMatrix(*obj);
            objData.baseColorAndPlanetRadius = glm::vec4(baseColor, 1.0f);
            objData.planetCenterAndFlag      = glm::vec4(0.0f);

            glBindBuffer(GL_UNIFORM_BUFFER, m_uboPerObject);
            glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(PerObjectUBOData), &objData);
            glBindBuffer(GL_UNIFORM_BUFFER, 0);

            switch (command.kind) {
                case Haruka::RenderKind::Model: {
                    Model* model = getOrLoadModelCached(obj->modelPath);
                    if (!model) break;
                    model->Draw(*_mainShader);
                    ++renderedDrawCalls;
                    renderedVertices  += model->getVertexCount();
                    renderedTriangles += model->getTriangleCount();
                    break;
                }
                case Haruka::RenderKind::MeshComponent: {
                    if (!obj->meshRenderer || !obj->meshRenderer->isResident()) break;
                    obj->meshRenderer->render(*_mainShader);
                    ++renderedDrawCalls;
                    renderedVertices  += obj->meshRenderer->getResidentVertexCount();
                    renderedTriangles += obj->meshRenderer->getResidentTriangleCount();
                    break;
                }
                case Haruka::RenderKind::Primitive: {
                    SimpleMesh* primitiveMesh = getPrimitiveMesh(command.primitive);
                    if (!primitiveMesh) break;
                    primitiveMesh->draw();
                    ++renderedDrawCalls;
                    renderedVertices  += primitiveMesh->getVertexCount();
                    renderedTriangles += primitiveMesh->getTriangleCount();
                    break;
                }
                case Haruka::RenderKind::None:
                    break;
            }
        }

        _iRenderedDrawCalls = renderedDrawCalls;
        _iRenderedVertices  = renderedVertices;
        _iRenderedTriangles = renderedTriangles;
        _iTotalVertices     = renderedVertices;
        _iTotalTriangles    = renderedTriangles;

        // Terrain streaming: update LOD + render planet chunks
        if (_planetarySystem) {
            _planetarySystem->syncFromScene(*_currentScene);
            _planetarySystem->update(0.016, glm::dvec3(_camera->position));

            _iVisibleChunks         = _planetarySystem->getGPUChunkCount();
            _iResidentChunks        = _planetarySystem->getCachedChunks();
            _iPendingChunkLoads     = _planetarySystem->getPendingChunks();
            _iPendingChunkEvictions = 0;
            _iTrackedChunks         = _iVisibleChunks + _iResidentChunks;
            _iResidentMemoryMB      = _planetarySystem->getCacheMemoryMB();
            _iMaxMemoryMB           = _planetarySystem->getCacheMaxMemoryMB();

            for (const auto& planet : _planetarySystem->getPlanets()) {
                glm::vec3 planetCenter = glm::vec3(planet.position) - cameraOrigin;

                PerObjectUBOData terrainObj{};
                terrainObj.model                    = glm::translate(glm::mat4(1.0f), planetCenter);
                terrainObj.baseColorAndPlanetRadius = glm::vec4(0.76f, 0.78f, 0.82f, (float)planet.radius);
                terrainObj.planetCenterAndFlag      = glm::vec4(0.0f); // already in model space

                glBindBuffer(GL_UNIFORM_BUFFER, m_uboPerObject);
                glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(PerObjectUBOData), &terrainObj);
                glBindBuffer(GL_UNIFORM_BUFFER, 0);

                _planetarySystem->renderPlanetTerrain(planet.name);
            }

            // Add terrain draw stats on top of regular object stats
            const auto ts = _planetarySystem->getTerrainDrawStats();
            _iRenderedDrawCalls += ts.draws;
            _iRenderedVertices  += ts.vertices;
            _iRenderedTriangles += ts.triangles;
            _iTotalDrawCalls    = _iRenderedDrawCalls;
            _iTotalVertices     = _iRenderedVertices;
            _iTotalTriangles    = _iRenderedTriangles;
        }
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

#ifdef HARUKA_NETWORK
void Application::sendPlayerTransform(uint32_t uuid, const Haruka::WorldPos& pos, const Haruka::Rotation& rot) {
    if (!m_dgs.isConnected()) return;
    int32_t cx = (int32_t)std::floor(pos.x / Haruka::Units::KM);
    int32_t cy = (int32_t)std::floor(pos.y / Haruka::Units::KM);
    int32_t cz = (int32_t)std::floor(pos.z / Haruka::Units::KM);
    float localPos[3] = {
        (float)(pos.x - cx * Haruka::Units::KM),
        (float)(pos.y - cy * Haruka::Units::KM),
        (float)(pos.z - cz * Haruka::Units::KM)
    };
    float rotF[4] = { (float)rot.x, (float)rot.y, (float)rot.z, (float)rot.w };
    m_dgs.sendTransform(uuid, cx, cy, cz, localPos, rotF);
}
#endif

void Application::cleanup() {
    g_sceneRenderQueue.clear();

    #ifdef HARUKA_NETWORK
        m_dgs.disconnect();
    #endif

    MotorInstance::getInstance().clear();

    _currentScene = nullptr;
    _ownedScene.reset();

    _physicsEngine.reset();
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