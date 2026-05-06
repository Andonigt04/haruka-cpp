/**
 * @file camera.h
 * @brief Quaternion-based runtime camera with SDL input handling.
 *
 * Position is stored as `Haruka::WorldPos` (dvec3) for double-precision
 * accuracy at astronomical distances. Orientation is a `Haruka::Rotation`
 * (dquat). The view matrix is derived from position + orientation each frame.
 *
 * Input is polled directly from SDL keyboard state in `processInput()`;
 * mouse rotation is applied via `rotate()` using delta values from the
 * SDL mouse-motion event.
 */
#ifndef CAMERA_H
#define CAMERA_H

#include <glm/gtc/matrix_transform.hpp>
#include "tools/math_types.h"

struct SDL_Window;

/**
 * @brief Runtime camera state and input-driven transform controller.
 */
class Camera {
public:

    Haruka::WorldPos position;    ///< World-space position (km, double precision)
    Haruka::Rotation orientation; ///< Absolute orientation as unit quaternion
    float speed = 5.0f;           ///< Movement speed in km/s
    float sensitivity = 0.1f;     ///< Mouse rotation sensitivity (degrees per pixel)
    float zoom = 45.0f;           ///< Vertical FOV in degrees

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
    /** @brief Processes movement input from SDL keyboard state. */
    void processInput(SDL_Window* window, float deltaTime);
    
    /** @brief Updates zoom/FOV from scroll input. */
    void ProcessMouseScroll(float yoffset);
};
#endif