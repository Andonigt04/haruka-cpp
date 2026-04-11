#include "camera.h"

#include <GLFW/glfw3.h>

Camera::Camera(Haruka::WorldPos startPos)
    : position(startPos), orientation(glm::dvec3(0.0, 0.0, 0.0)), zoom(45.0f) {}

glm::vec3 Camera::getFront() const {
    return glm::normalize(orientation * glm::dvec3(0.0f, 0.0f, -1.0f));
}

glm::vec3 Camera::getUp() const {
    return glm::normalize(orientation * glm::dvec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 Camera::getViewMatrix() const {
    return glm::lookAt(Haruka::LocalPos(position), Haruka::LocalPos(position) + getFront(), getUp());
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

void Camera::processInput(GLFWwindow* window, float deltaTime) {
    double velocity = (double)speed * (double)deltaTime;
    glm::vec3 right = glm::normalize(glm::cross(getFront(), getUp()));
    
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) position += Haruka::WorldPos(getFront()) * velocity;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) position -= Haruka::WorldPos(getFront()) * velocity;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) position -= Haruka::WorldPos(right) * velocity;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) position += Haruka::WorldPos(right) * velocity;
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) position += Haruka::WorldPos(getUp()) * velocity;
    if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) position -= Haruka::WorldPos(getUp()) * velocity;
}

void Camera::ProcessMouseScroll(float yoffset) {
    zoom -= (float)yoffset;
    if (zoom < 1.0f) zoom = 1.0f;
    if (zoom > 45.0f) zoom = 45.0f;
}

glm::mat4 Camera::getProjectionMatrix() const {
    return glm::perspective(glm::radians(zoom), 16.0f / 9.0f, 0.1f, 100.0f);
}
