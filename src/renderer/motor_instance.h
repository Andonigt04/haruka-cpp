#ifndef MOTOR_INSTANCE_H
#define MOTOR_INSTANCE_H

class Application;
class RenderTarget;
class Camera;
class IEngine;

namespace Haruka {
    class Scene;
}

/**
 * @brief Singleton bridge between the editor and the running motor/runtime.
 *
 * Exposes the active render target, scene, camera, and application pointers
 * to editor and scripting code without transferring ownership.
 */
class MotorInstance {
    friend class Application;
    
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
    void setScene(Haruka::Scene* scene) {
        motorScene = scene;
    }
    
    /** @brief Stores the active camera pointer. */
    void setCamera(Camera* cam) {
        motorCamera = cam;
    }
    
    /** @brief Stores the active application pointer. */
    void setApplication(Application* app) {
        motorApplication = app;
    }

    /** @brief Stores the active engine pointer. */
    void setEngine(IEngine* eng) { motorEngine = eng; }
    /** @brief Returns the active engine pointer. */
    IEngine* getEngine() const { return motorEngine; }

    /** @brief Returns the active render target pointer. */
    RenderTarget* getRenderTarget() const {
        return motorRenderTarget;
    }
    
    /** @brief Returns the active scene pointer. */
    Haruka::Scene* getScene() const {
        return motorScene;
    }
    
    /** @brief Returns the active camera pointer. */
    Camera* getCamera() const {
        return motorCamera;
    }
    
    /** @brief Returns the active application pointer. */
    Application* getApplication() const {
        return motorApplication;
    }
    
    /** @brief Returns true when runtime pointers are available. */
    bool isMotorActive() const {
        return motorScene != nullptr && motorRenderTarget != nullptr;
    }
    
    /** @brief Clears non-owning runtime pointers. */
    void clear() {
        motorScene = nullptr;
        motorCamera = nullptr;
        motorApplication = nullptr;
        motorEngine = nullptr;
    }

private:
    MotorInstance() = default;
    ~MotorInstance() = default;

    MotorInstance(const MotorInstance&) = delete;
    MotorInstance& operator=(const MotorInstance&) = delete;

    RenderTarget* motorRenderTarget = nullptr;
    Haruka::Scene* motorScene = nullptr;
    Camera* motorCamera = nullptr;
    Application* motorApplication = nullptr;
    IEngine* motorEngine = nullptr;
};

#endif