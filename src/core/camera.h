#ifndef CAMERA_H
#define CAMERA_H

#include <glm/gtc/matrix_transform.hpp>

#include "math_types.h"


/**
 * @brief Runtime camera state and input-driven transform controller.
 */
class Camera {
public:

    Haruka::WorldPos position;
    Haruka::Rotation orientation;
    float speed = 5.0f;
    float sensitivity = 0.1f;
    float zoom = 45.0f;

    /** @brief Constructs camera at initial world-space position. */
    Camera(Haruka::WorldPos startPos);
    
    /** @brief Returns current forward direction vector. */
    glm::vec3 getFront() const;
    /** @brief Returns camera up vector from current orientation. */
    glm::vec3 getUp() const;
    /** @brief Returns view matrix from position/orientation state. */
    glm::mat4 getViewMatrix() const;
    /** @brief Returns projection matrix state. */
    glm::mat4 getProjectionMatrix() const;

    /** @brief Applies mouse-delta rotation update. */
    void rotate(float deltaX, float deltaY);
    /** @brief Processes movement input state. */
    void processInput(float deltaTime);
    
    /** @brief Updates zoom/FOV from scroll input. */
    void ProcessMouseScroll(float yoffset);
};
#endif