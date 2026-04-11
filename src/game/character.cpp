#define GLM_ENABLE_EXPERIMENTAL
#include "character.h"
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <iostream>
#include <algorithm>
#include <glm/gtx/quaternion.hpp>

namespace Haruka {

namespace {
glm::dvec3 safeNormalize(const glm::dvec3& v, const glm::dvec3& fallback) {
    double len = glm::length(v);
    if (len < 1e-9) return fallback;
    return v / len;
}

void buildSurfaceBasis(
    const glm::dvec3& gravityUp,
    const glm::quat& cameraOrientation,
    glm::dvec3& outUp,
    glm::dvec3& outForward,
    glm::dvec3& outRight
) {
    outUp = safeNormalize(gravityUp, glm::dvec3(0.0, 1.0, 0.0));

    glm::vec3 camForward3 = cameraOrientation * glm::vec3(0, 0, -1);
    glm::dvec3 camForward = glm::dvec3(camForward3);

    // Proyectar la cámara sobre el plano tangente del planeta
    outForward = camForward - outUp * glm::dot(camForward, outUp);
    outForward = safeNormalize(outForward, glm::cross(glm::dvec3(0.0, 0.0, 1.0), outUp));

    outRight = glm::cross(outForward, outUp);
    outRight = safeNormalize(outRight, glm::dvec3(1.0, 0.0, 0.0));
}
}

Character::Character(const glm::dvec3& position, const std::string& userId)
    : userId(userId), position(position), velocity(0.0) {
    
    localPlayer = !userId.empty();
    
    if (localPlayer) {
        camera = std::make_unique<Camera>(WorldPos(position.x, position.y + currentHeight * 0.9, position.z));
    }
    
    forward = glm::vec3(0, 0, -1);
    right = glm::vec3(1, 0, 0);
    // Si planeta está en (0,0,0), es simplemente normalize(position)
    double posLen = glm::length(position);
    upDirection = posLen > 1e-6 ? (position / posLen) : glm::dvec3(0.0, 1.0, 0.0);
    
    lastSyncPos = position;
    lastSyncRot = glm::vec3(yaw, pitch, 0);
    targetPosition = position;
    targetRotation = glm::vec3(yaw, pitch, 0);
    
    std::cout << "[Character] Created" << (localPlayer ? " (local)" : " (remote)") << " at " 
              << position.x << ", " << position.y << ", " << position.z << std::endl;
}

Character::~Character() {}

void Character::update(float deltaTime) {
    // Sincronizar con física si existe
    if (physicsBody) {
        position = glm::dvec3(physicsBody->position);
        velocity = glm::dvec3(physicsBody->velocity);
    }
    
    // Interpolación para jugadores remotos
    if (!localPlayer) {
        position = glm::mix(position, targetPosition, deltaTime * interpolationSpeed);
        
        float targetYaw = targetRotation.x;
        float targetPitch = targetRotation.y;
        yaw = glm::mix(yaw, targetYaw, deltaTime * interpolationSpeed);
        pitch = glm::mix(pitch, targetPitch, deltaTime * interpolationSpeed);
    }
    
    checkGrounded();
    updateState();
    
    if (localPlayer) {
        updateCamera();
        
        // Sync con servidor
        syncTimer += deltaTime;
        if (syncTimer >= syncInterval && networkClient) {
            if (shouldSyncToServer()) {
                syncToServer();
                syncTimer = 0.0f;
            }
        }
    }
    
    // Smooth crouching
    float targetHeight = crouched ? crouchingHeight : standingHeight;
    currentHeight += (targetHeight - currentHeight) * deltaTime * 10.0f;
}

void Character::processInput(GLFWwindow* window, float deltaTime) {
    if (!localPlayer) return;
    
    // Movement
    float speed = sprinting ? runSpeed : (crouched ? crouchSpeed : walkSpeed);

    glm::dvec3 up, surfaceForward, surfaceRight;
    glm::dquat camOrientation = camera ? camera->orientation : glm::dquat(1.0, 0.0, 0.0, 0.0);
    buildSurfaceBasis(getEffectiveUp(), camOrientation, up, surfaceForward, surfaceRight);
    
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
        position += surfaceForward * (double)(speed * deltaTime);
    }
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
        position -= surfaceForward * (double)(speed * deltaTime);
    }
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
        position -= surfaceRight * (double)(speed * deltaTime);
    }
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
        position += surfaceRight * (double)(speed * deltaTime);
    }
    
    // Jump
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS && grounded) {
        jump();
    }
    
    // Sprint
    sprint(glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS);
    
    // Crouch
    crouch(glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS);
}

void Character::moveForward(float amount) {
    position += glm::dvec3(forward) * (double)amount;
}

void Character::moveRight(float amount) {
    position += glm::dvec3(right) * (double)amount;
}

void Character::jump() {
    if (grounded && physicsBody) {
        physicsBody->velocity.y = jumpForce;
        grounded = false;
    }
}

void Character::crouch(bool enabled) {
    crouched = enabled;
}

void Character::sprint(bool enabled) {
    sprinting = enabled && !crouched;
}

void Character::rotate(float yawDelta, float pitchDelta) {
    if (!localPlayer) return;
    
    yaw += yawDelta * mouseSensitivity;
    pitch += pitchDelta * mouseSensitivity;

    if (minPitch > maxPitch) std::swap(minPitch, maxPitch);
    pitch = glm::clamp(pitch, minPitch, maxPitch);
    
    // Update forward/right vectors
    glm::vec3 direction;
    direction.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
    direction.y = sin(glm::radians(pitch));
    direction.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
    forward = glm::normalize(direction);
    
    right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
}

void Character::updateCamera() {
    if (!camera) return;

    glm::dvec3 up = getEffectiveUp();
    glm::dvec3 cameraPos = position + up * (double)(currentHeight * 0.9f);
    camera->position = WorldPos(cameraPos.x, cameraPos.y, cameraPos.z);

    // Orientación relativa al planeta (up local), para sensación humana sobre esfera
    glm::dvec3 referenceForward = glm::dvec3(0.0, 0.0, -1.0);
    if (std::abs(glm::dot(referenceForward, up)) > 0.95) {
        referenceForward = glm::dvec3(1.0, 0.0, 0.0);
    }

    glm::dvec3 tangentForward = safeNormalize(referenceForward - up * glm::dot(referenceForward, up), glm::dvec3(1.0, 0.0, 0.0));
    glm::dvec3 tangentRight = safeNormalize(glm::cross(tangentForward, up), glm::dvec3(0.0, 0.0, 1.0));

    glm::dquat yawQ = glm::angleAxis(glm::radians((double)yaw), up);
    glm::dvec3 forwardAfterYaw = glm::normalize(yawQ * tangentForward);
    glm::dvec3 rightAfterYaw = safeNormalize(glm::cross(forwardAfterYaw, up), tangentRight);

    glm::dquat pitchQ = glm::angleAxis(glm::radians((double)pitch), rightAfterYaw);
    glm::dvec3 finalForward = glm::normalize(pitchQ * forwardAfterYaw);

    glm::dvec3 finalRight = safeNormalize(glm::cross(finalForward, up), rightAfterYaw);
    glm::dvec3 finalUp = safeNormalize(glm::cross(finalRight, finalForward), up);

    glm::dmat3 basis;
    basis[0] = finalRight;
    basis[1] = finalUp;
    basis[2] = -finalForward;
    camera->orientation = glm::normalize(glm::quat_cast(basis));
}

glm::dvec3 Character::getEffectiveUp() const {
    glm::dvec3 up = safeNormalize(upDirection, glm::dvec3(0.0, 1.0, 0.0));
    if (glm::length(up) < 1e-9) {
        up = safeNormalize(position, glm::dvec3(0.0, 1.0, 0.0));
    }
    return up;
}

void Character::updateState() {
    float horizontalSpeed = glm::length(glm::vec2(velocity.x, velocity.z));
    
    if (!grounded) {
        state = velocity.y > 0 ? CharacterState::JUMPING : CharacterState::FALLING;
    } else if (crouched) {
        state = CharacterState::CROUCHING;
    } else if (horizontalSpeed > 0.1f) {
        state = sprinting ? CharacterState::RUNNING : CharacterState::WALKING;
    } else {
        state = CharacterState::IDLE;
    }
}

void Character::checkGrounded() {
    if (physicsBody) {
        grounded = (physicsBody->velocity.y < 0.1f && physicsBody->velocity.y > -0.1f);
    }
}

bool Character::shouldSyncToServer() {
    float posDelta = glm::length(glm::dvec3(position) - lastSyncPos);
    float rotDelta = glm::length(glm::vec3(yaw, pitch, 0) - lastSyncRot);
    
    return posDelta > syncThreshold || rotDelta > syncThreshold;
}

void Character::syncToServer() {
    if (!networkClient || !localPlayer) return;
    
    networkClient->sendPositionUpdate(
        glm::dvec3(position),
        glm::vec3(yaw, pitch, 0)
    );
    
    lastSyncPos = position;
    lastSyncRot = glm::vec3(yaw, pitch, 0);
    
    std::cout << "[Character] Synced to server: " 
              << position.x << ", " << position.y << ", " << position.z << std::endl;
}

void Character::applyServerUpdate(const glm::dvec3& serverPos, const glm::vec3& serverRot) {
    double distance = glm::length(serverPos - position);
    
    if (distance > 1.0) {
        targetPosition = serverPos;
        targetRotation = serverRot;
    }
}

}