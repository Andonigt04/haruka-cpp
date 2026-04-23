#pragma once


#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <memory>
#include "core/camera.h"
#include "physics/physics_engine.h"
#include "network/network_manager.h"

namespace Haruka {

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
    /** @brief Processes keyboard/mouse input from SDL window and event. */
    void processInput(SDL_Window* window, const Uint8* keyState, float deltaTime);
    
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
    ///@}
    
    /** @name Network synchronization */
    ///@{
    void setNetworkClient(NetworkClient* client) { networkClient = client; }
    void syncToServer();
    void applyServerUpdate(const glm::dvec3& serverPos, const glm::vec3& serverRot);
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
    
    bool isGrounded() const { return grounded; }
    bool isSprinting() const { return sprinting; }
    bool isCrouching() const { return crouched; }
    bool isLocalPlayer() const { return localPlayer; }
    bool isInFlightMode() const { return flightMode; }
    ///@}
    
    void setFlightMode(bool enabled) { flightMode = enabled; }

private:
    std::string userId;
    bool localPlayer = true;
    
    glm::dvec3 position;
    glm::dvec3 velocity;
    glm::vec3 forward;
    glm::vec3 right;
    
    std::unique_ptr<Camera> camera;
    std::shared_ptr<RigidBody> physicsBody;
    NetworkClient* networkClient = nullptr;
    
    CharacterState state = CharacterState::IDLE;
    
    // Movement parameters
    float walkSpeed = 0.05f;
    float runSpeed = 0.12f;
    float crouchSpeed = 0.02f;
    float jumpForce = 0.03f;
    float mouseSensitivity = 0.1f;
    
    float yaw = -90.0f;
    float pitch = 0.0f;
    float minPitch = -85.0f;
    float maxPitch = 85.0f;

    glm::dvec3 upDirection = glm::dvec3(0.0, 1.0, 0.0);
    
    bool grounded = false;
    bool sprinting = false;
    bool crouched = false;
    bool flightMode = false;
    
    float standingHeight = 0.0019f;
    float crouchingHeight = 0.0012f;
    float currentHeight = 0.0019f;
    
    // Network sync
    glm::dvec3 lastSyncPos;
    glm::vec3 lastSyncRot;
    glm::dvec3 targetPosition;
    glm::vec3 targetRotation;
    float syncThreshold = 0.1f;
    float syncInterval = 0.1f;
    float syncTimer = 0.0f;
    float interpolationSpeed = 5.0f;
    
    /** @brief Updates the attached camera from current character state. */
    void updateCamera();
    /** @brief Updates locomotion state from velocity/input context. */
    void updateState();
    /** @brief Recomputes grounded state from physics/orientation. */
    void checkGrounded();
    /** @brief Returns true when a network sync should be emitted. */
    bool shouldSyncToServer();

    /** @brief Returns the current up vector used for movement basis. */
    glm::dvec3 getEffectiveUp() const;
};

}