/**
 * @file MotorInstance.h
 * @brief Singleton bridge exposing active render target, scene, camera, and app to editor code.
 */
#pragma once

namespace Haruka { namespace Core { class Application; class Camera; } }
namespace Haruka { namespace Renderer { class RenderTarget; } }

namespace Haruka::Core::Scene {
    class SceneManager;
} // namespace Haruka::Core::Scene

namespace Haruka { namespace Renderer {

/**
 * @brief Singleton bridge between the editor and the running motor/runtime.
 *
 * Exposes the active render target, scene, camera, and application pointers
 * to editor and scripting code without transferring ownership.
 */
class MotorInstance {
    friend class Haruka::Core::Application;
    
public:
    /** @brief Returns the singleton instance. */
    static MotorInstance& getInstance() {
        static MotorInstance instance;
        return instance;
    }
    
    /** @brief Stores the active render target pointer. */
    void setRenderTarget(RenderTarget* target) {
        motorRenderTarget = target;
    }
    
    /** @brief Stores the active scene pointer. */
    void setScene(Core::Scene::SceneManager* scene) {
        motorScene = scene;
    }
    
    /** @brief Stores the active camera pointer. */
    void setCamera(Core::Camera* cam) {
        motorCamera = cam;
    }
    
    /** @brief Stores the active application pointer. */
    void setApplication(Core::Application* app) {
        motorApplication = app;
    }
    
    /** @brief Returns the active render target pointer. */
    RenderTarget* getRenderTarget() const {
        return motorRenderTarget;
    }
    
    /** @brief Returns the active scene pointer. */
    Core::Scene::SceneManager* getScene() const {
        return motorScene;
    }
    
    /** @brief Returns the active camera pointer. */
    Core::Camera* getCamera() const {
        return motorCamera;
    }
    
    /** @brief Returns the active application pointer. */
    Core::Application* getApplication() const {
        return motorApplication;
    }
    
    /** @brief Returns true when runtime pointers are available. */
    bool isMotorActive() const {
        return motorScene != nullptr && motorRenderTarget != nullptr;
    }

    void setPlayMode(bool play) { motorPlayMode = play; }
    bool isPlayMode() const     { return motorPlayMode; }

    /** @brief Clears non-owning runtime pointers. */
    void clear() {
        motorScene = nullptr;
        motorCamera = nullptr;
        motorApplication = nullptr;
        motorPlayMode = false;
    }

private:
    MotorInstance() = default;
    ~MotorInstance() = default;

    // Prevenir copia
    MotorInstance(const MotorInstance&) = delete;
    MotorInstance& operator=(const MotorInstance&) = delete;

    RenderTarget* motorRenderTarget = nullptr;
    Core::Scene::SceneManager* motorScene = nullptr;
    Core::Camera* motorCamera = nullptr;
    Core::Application* motorApplication = nullptr;
    bool motorPlayMode = false;
};

}} // namespace Haruka::Renderer

             // back-compat alias (migration)
