#include "camera.h"
#include <cmath>   // std::tan (proyección reversed-Z)
#include "rhi/rhi_device.h"   // backend activo: la Y de Vulkan va al revés

#include <SDL3/SDL.h>

namespace Haruka { namespace Core {

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
    Haruka::WorldPos pos = position;
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



void Camera::ProcessMouseScroll(float yoffset) {
    zoom -= (float)yoffset;
    if (zoom < 1.0f) zoom = 1.0f;
    if (zoom > 45.0f) zoom = 45.0f;
}

// --- REVERSED-Z con FAR INFINITO ---------------------------------------------------------
// Antes: glm::perspective(fov, aspect, 0.1, 3e11). A escala planetaria eso destruía la precisión
// de profundidad: TODO el terreno caía entre 0.99996 y 1.0 (medido) → el z-buffer no distinguía el
// mar del suelo (z-fighting, y el agua sin poder ocluirse por depth → tenía que recortar la costa
// re-evaluando el ruido del terreno per-píxel).
//
// Reversed-Z: near mapea a 1 y el infinito a 0. Como el float tiene MUCHA más resolución cerca de
// 0, la precisión se reparte bien por todo el rango → profundidad utilizable a escala de planeta.
// Requisitos (ya puestos): glClipControl(ZERO_TO_ONE), depth buffer D32F, clear de depth a 0 y
// comparación GREATER/GEQUAL (los PSO lo traen por defecto; ver rhi_resources.h).
//
// Matriz (columna-mayor; f = 1/tan(fov/2)):
//   [ f/aspect  0    0     0 ]
//   [    0      f    0     0 ]
//   [    0      0    0    -1 ]
//   [    0      0   near   0 ]
// z_ndc = near/(-z_view) → -z_view = near ⇒ 1 ; -z_view → ∞ ⇒ 0. Sin plano lejano: nada se recorta.
static glm::mat4 reversedZInfinitePerspective(float fovYRadians, float aspect, float zNear) {
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);
    glm::mat4 p(0.0f);
    p[0][0] = f / aspect;
    p[1][1] = f;
    p[2][3] = -1.0f;
    p[3][2] = zNear;

    // ⚠️ EL EJE Y DE VULKAN VA AL REVÉS QUE EL DE OPENGL, y se compensa AQUÍ.
    //
    // En Vulkan la Y del clip apunta hacia abajo: con la proyección de GL el mundo se rasteriza
    // espejado en vertical. La compensación "de manual" es un viewport de altura NEGATIVA, y se
    // PROBÓ Y NO SIRVE en este motor: el viewport afecta a TODOS los pases, y un pase de
    // post-proceso dibuja un quad a pantalla completa muestreando una textura — invertirlo espeja
    // la imagen otra vez. Con el bloom iterando un número configurable de veces, la PARIDAD de
    // espejados cambiaba y el frame salía derecho o del revés ALTERNANDO.
    //
    // Invirtiendo la proyección solo se toca lo que SE PROYECTA (la geometría 3D). Los quads a
    // pantalla completa, que van directos en NDC, quedan intactos.
    if (Haruka::RHI::Device* dev = Haruka::RHI::device())
        if (dev->backend() == Haruka::RHI::Backend::Vulkan)
            p[1][1] = -f;

    return p;
}

glm::mat4 Camera::getProjectionMatrix() const {
    return reversedZInfinitePerspective(glm::radians(zoom), 16.0f / 9.0f, m_nearPlane);
}

glm::mat4 Camera::getProjectionMatrix(float aspectRatio) const {
    return reversedZInfinitePerspective(glm::radians(zoom), aspectRatio, m_nearPlane);
}


}} // namespace Haruka::Core
