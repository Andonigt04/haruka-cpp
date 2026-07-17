/**
 * @file character.h
 * @brief Player/NPC controller with camera, physics body, and network sync hooks.
 */
#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <functional>
#include <vector>
#include "core/camera.h"
#include "physics/physics_engine.h"
#include "tools/math_types.h"
#include "input/input_action.h"

namespace Haruka {

/** When an action callback fires. */
enum class ActionTrigger {
    Performed,    // every frame the action is active (held)
    Started,      // first frame it becomes active
    Canceled,     // first frame it stops being active
};

/**
 * Input query functions injected from game code.
 * Character never touches SettingsManager or SDL directly.
 */
struct CharacterInputProvider {
    std::function<Input::ActionValue(const std::string&)> readValue;
    std::function<bool(const std::string&)>               isPerformed;
    std::function<bool(const std::string&)>               isStarted;
    std::function<bool(const std::string&)>               isCanceled;
};

/** @brief Character locomotion and sync state machine. */
enum class CharacterState {
    IDLE,
    WALKING,
    RUNNING,
    JUMPING,
    FALLING,
    CROUCHING
};

/**
 * @brief Player/NPC controller with camera, physics, and network sync hooks.
 */
class Character {
public:
    /** @brief Constructs a character at the given world position. */
    Character(const glm::dvec3& position, const std::string& userId = "");
    /** @brief Releases owned character resources. */
    ~Character();
    
    /** @brief Advances movement, camera, and sync state. */
    void update(float deltaTime);
    /** @brief Processes keyboard/mouse input from SDL window. */
    void processInput(SDL_Window* window, float deltaTime);
    
    /** @name Movement controls */
    ///@{
    void moveForward(float amount);
    void moveRight(float amount);
    void jump();
    void crouch(bool enabled);
    void sprint(bool enabled);
    ///@}
    
    /** @name Camera controls */
    ///@{
    void rotate(float yaw, float pitch);
    Camera* getCamera() const { return camera.get(); }
    ///@}
    
    /** @name Physics binding */
    ///@{
    void setPhysicsBody(std::shared_ptr<RigidBody> body) { physicsBody = body; }
    std::shared_ptr<RigidBody> getPhysicsBody() { return physicsBody; }
    /** @brief (Fase 2) Movimiento GESTIONADO POR EL MOTOR: en vez de escribir la posición a mano,
     *  move() fija la velocidad de locomoción en el body y el motor integra (gravedad radial +
     *  ground-snap). Así una fuerza externa (viento, empujón, magia) SÍ afecta al jugador. Off =
     *  comportamiento clásico (kinemático, el juego integra a mano). Lo activa Player según un flag. */
    void setPhysicsDriven(bool on) { m_physicsDriven = on; }
    bool isPhysicsDriven() const { return m_physicsDriven; }
    ///@}
    
    /** @name Network synchronization */
    ///@{
    void applyServerUpdate(const Haruka::WorldPos& serverPos, const Haruka::Rotation& serverRot);

    std::function<void(const WorldPos&, const Rotation&)> onTransformChanged;
    ///@}
    
    /** @name State accessors */
    ///@{
    glm::dvec3 getPosition() const { return position; }
    glm::dvec3 getVelocity() const { return velocity; }
    glm::dvec3 getUpDirection() const { return upDirection; }
    CharacterState getState() const { return state; }
    std::string getUserId() const { return userId; }

    void setPosition(glm::dvec3 newPosition) { position = newPosition; }
    void setVelocity(glm::dvec3 newVelocity) { velocity = newVelocity; }
    void setState(CharacterState newState) { state = newState; }
    void setUpDirection(const glm::dvec3& newUp) { upDirection = newUp; }
    void setPitchLimits(float minPitchDeg, float maxPitchDeg) { minPitch = minPitchDeg; maxPitch = maxPitchDeg; }
    /** @brief Sets movement speeds in m/s (walk, run/sprint, crouch). */
    void setMoveSpeeds(float walk, float run, float crouch) {
        walkSpeed = walk; runSpeed = run; crouchSpeed = crouch;
    }
    float getWalkSpeed() const { return walkSpeed; }
    // Jump impulse (m/s). Also reused as the ascend/descend speed in flight mode.
    void  setJumpForce(float f) { jumpForce = f; }
    float getJumpForce() const  { return jumpForce; }

    bool isGrounded() const { return grounded; }
    bool isSprinting() const { return sprinting; }
    bool isCrouching() const { return crouched; }
    bool isLocalPlayer() const { return localPlayer; }
    bool isInFlightMode() const { return flightMode; }
    // Called by the game's surface constraint each frame; marks grounding as
    // externally managed so checkGrounded() stops using the (sphere-wrong) Y test.
    void setGrounded(bool g) { grounded = g; m_externalGround = true; }
    ///@}
    
    void setFlightMode(bool enabled) { flightMode = enabled; }

    // -- Input system --------------------------------------------------------

    void setInputProvider(const CharacterInputProvider& p) { m_inputProvider = p; }

    // Register a callback for any action/trigger combination.
    // T must match the ActionValue type the action produces (bool, float, vec2, vec3).
    // Use the value-less overload for simple button presses.
    template<typename T>
    void bindAction(const std::string& action, ActionTrigger trigger,
                    std::function<void(Character&, float, T)> cb)
    {
        m_bindings.push_back({ action, trigger,
            [cb](Character& c, float dt, const Input::ActionValue& v) {
                if (const T* p = std::get_if<T>(&v)) cb(c, dt, *p);
            }
        });
    }

    // Value-less overload — for actions where you only care that it fired.
    void bindAction(const std::string& action, ActionTrigger trigger,
                    std::function<void(Character&, float)> cb)
    {
        m_bindings.push_back({ action, trigger,
            [cb](Character& c, float dt, const Input::ActionValue&) { cb(c, dt); }
        });
    }

    void clearBindings() { m_bindings.clear(); }

    // Convenience movement method for use in bindAction callbacks.
    // input.y = forward(-1..1), input.x = strafe(-1..1).
    // Applies sprint/crouch speed automatically.
    void move(glm::vec2 input, float deltaTime);

    float getSpeed() const;

    /** Sync camera position/orientation from current yaw/pitch/position.
     *  Call after rotate() so the view matrix reflects the new direction
     *  without waiting for the next update() tick. */
    void updateCamera();

private:
    // Type-erased action binding
    struct ActionBinding {
        std::string action;
        ActionTrigger trigger;
        std::function<void(Character&, float, const Input::ActionValue&)> callback;
    };

    CharacterInputProvider       m_inputProvider;
    std::vector<ActionBinding>   m_bindings;
    std::string userId;
    bool localPlayer = true;
    
    glm::dvec3 position;
    glm::dvec3 velocity;
    glm::vec3 forward;
    glm::vec3 right;
    
    std::unique_ptr<Camera> camera;
    std::shared_ptr<RigidBody> physicsBody;
    bool m_physicsDriven = false;   // (Fase 2) el motor integra el body; move() fija velocidad, no posición
    
    CharacterState state = CharacterState::IDLE;
    
    // Movement parameters (meters / second)
    float walkSpeed = 1.5f;
    float runSpeed = 4.0f;
    float crouchSpeed = 0.6f;
    float jumpForce = 5.0f;
    float mouseSensitivity = 0.1f;
    
    float yaw = -90.0f;
    float pitch = 0.0f;
    float minPitch = -85.0f;
    float maxPitch = 85.0f;

    glm::dvec3 upDirection = glm::dvec3(0.0, 1.0, 0.0);
    
    bool grounded = false;
    bool m_externalGround = false; // grounded managed by the game's surface constraint
    bool sprinting = false;
    bool crouched = false;
    bool flightMode = false;
    
    // Character capsule height (meters)
    float standingHeight = 1.9f;
    float crouchingHeight = 1.2f;
    float currentHeight = 1.9f;
    
    // Network sync
    Haruka::WorldPos lastSyncPos;
    Haruka::Rotation lastSyncRot;
    Haruka::WorldPos targetPosition;
    Haruka::Rotation targetRotation;
    float syncThreshold = 0.1f;
    float syncInterval = 0.1f;
    float syncTimer = 0.0f;
    float interpolationSpeed = 5.0f;
    
    /** @brief Updates locomotion state from velocity/input context. */
    void updateState();
    /** @brief Recomputes grounded state from physics/orientation. */
    void checkGrounded();

    /** @brief Returns the current up vector used for movement basis. */
    glm::dvec3 getEffectiveUp() const;
};

}