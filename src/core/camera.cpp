#include "camera.h"

#include <SDL3/SDL.h>

Camera::Camera(Haruka::WorldPos startPos)
    : position(startPos), orientation(glm::dvec3(0.0, 0.0, 0.0)), zoom(45.0f) {}

glm::vec3 Camera::getFront() const {
    return glm::normalize(orientation * glm::dvec3(0.0f, 0.0f, -1.0f));
}

glm::vec3 Camera::getUp() const {
    return glm::normalize(orientation * glm::dvec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 Camera::getViewMatrix() const {
    // Use double-precision lookAt so the forward direction (pos + front) doesn't
    // get lost to float rounding at large world positions (e.g. 1.5e8 units).
    // The resulting dmat4 is then narrowed to mat4 for the shader.
    glm::dvec3 pos   = position;
    glm::dvec3 front = glm::dvec3(getFront());
    glm::dvec3 up    = glm::dvec3(getUp());
    return glm::mat4(glm::lookAt(pos, pos + front, up));
}

void Camera::rotate(float deltaX, float deltaY) {
    // Change to radians and apply sensitivity
    double xRad = glm::radians(-(double)deltaX * sensitivity);
    double yRad = glm::radians(-(double)deltaY * sensitivity);
    
    // Where rotate camera to
    glm::dvec3 yawAxis(0, 1, 0);
    glm::dvec3 pitchAxis(1, 0, 0);

    orientation *= glm::angleAxis(xRad, yawAxis);
    orientation *= glm::angleAxis(yRad, pitchAxis);
}

void Camera::processInput(SDL_Window* /*window*/, float deltaTime) {
    double velocity = (double)speed * (double)deltaTime;
    glm::vec3 right = glm::normalize(glm::cross(getFront(), getUp()));

    const bool* keys = SDL_GetKeyboardState(nullptr);
    if (keys[SDL_SCANCODE_W])     position += Haruka::WorldPos(getFront()) * velocity;
    if (keys[SDL_SCANCODE_S])     position -= Haruka::WorldPos(getFront()) * velocity;
    if (keys[SDL_SCANCODE_A])     position -= Haruka::WorldPos(right) * velocity;
    if (keys[SDL_SCANCODE_D])     position += Haruka::WorldPos(right) * velocity;
    if (keys[SDL_SCANCODE_SPACE]) position += Haruka::WorldPos(getUp()) * velocity;
    if (keys[SDL_SCANCODE_LCTRL]) position -= Haruka::WorldPos(getUp()) * velocity;
}

void Camera::ProcessMouseScroll(float yoffset) {
    zoom -= (float)yoffset;
    if (zoom < 1.0f) zoom = 1.0f;
    if (zoom > 45.0f) zoom = 45.0f;
}

glm::mat4 Camera::getProjectionMatrix() const {
    return glm::perspective(glm::radians(zoom), 16.0f / 9.0f, 0.1f, 100.0f);
}
