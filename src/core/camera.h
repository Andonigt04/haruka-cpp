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

namespace Haruka { namespace Core {

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
    float aspectRatio = 1.0f;     ///< Aspect ratio for projection matrix
    float m_nearPlane = 0.1f;     ///< Near plane (m). DINÁMICO: la app lo sube con la altitud
                                  ///< (órbita) → precisión de depth lejana ~millones× mejor (sin z-fight
                                  ///< agua/lecho); pegado al suelo se queda en 0.1 (precisión cercana).

    /** @brief Fija el near plane (la app lo calcula por frame desde la altitud sobre el planeta). */
    void setNearPlane(float n) { m_nearPlane = (n > 0.001f) ? n : 0.001f; }
    float getNearPlane() const { return m_nearPlane; }

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
    /** @brief Returns projection matrix with specified aspect ratio. */
    glm::mat4 getProjectionMatrix(float aspectRatio) const;
    /** @brief Applies mouse-delta rotation update. */
    void rotate(float deltaX, float deltaY);
    /** @brief Sets the projection matrix ratio. */
    void setAspectRatio();
    /** @brief Sets the aspect ratio for projection matrix calculations. */
    void setAspectRatio(float ap) { aspectRatio = ap; }

    
    /** @brief Updates zoom/FOV from scroll input. */
    void ProcessMouseScroll(float yoffset);
};

}} // namespace Haruka::Core

using Haruka::Core::Camera;
namespace Haruka { using Core::Camera; }
#endif