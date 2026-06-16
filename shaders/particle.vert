/**
 * @file particle.vert
 * @brief Partículas como GL_POINTS, camera-relativo (aPos = posición - cámara). Tamaño
 *        por distancia. Color + alpha por partícula (fade). Lo usa el sistema de efectos
 *        del juego (cosecha, etc.).
 */
#version 450 core

layout(location = 0) in vec3  aPos;    // relativo a la cámara
layout(location = 1) in vec3  aColor;
layout(location = 2) in float aAlpha;
layout(location = 3) in float aSize;   // metros

layout(location = 0) out vec3  vColor;
layout(location = 1) out float vAlpha;

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 _cameraPos;      float _pad0;
    vec3 sunDirection;    float _pad1;
    vec3 sunLightColor;   float ambientStrength;
    int  enableHDR;
};

void main() {
    vColor = aColor;
    vAlpha = aAlpha;
    vec4 clip = projection * mat4(mat3(view)) * vec4(aPos, 1.0);
    gl_Position  = clip;
    gl_PointSize = clamp(aSize * 600.0 / max(clip.w, 0.1), 2.0, 64.0); // más grande de cerca
}
