/**
 * @file shadow_depth.vert
 * @brief Pase de sombra: profundidad desde el sol. Geometría camera-relativa (igual que
 *        props/terreno: u_offset + aPos), proyectada por la matriz de luz.
 */
#version 450 core

layout(location = 0) in vec3 aPos;

layout(location = 10) uniform vec3 u_offset;     // origen - cámara
layout(location = 11) uniform mat4 u_lightSpace; // ortho*view del sol (camera-relativo)

void main() {
    gl_Position = u_lightSpace * vec4(u_offset + aPos, 1.0);
}
