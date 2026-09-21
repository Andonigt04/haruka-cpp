#version 460 core
#extension GL_GOOGLE_include_directive : require
// Triangulo de pantalla completa sin VBO: cubre el mapa de presion entero.
#include "lib/backend.glsl"
layout(location = 0) out vec2 vUv;
void main() {
    const int i = int(VERTEX_INDEX);
    vUv = vec2((i << 1) & 2, i & 2);
    gl_Position = vec4(vUv * 2.0 - 1.0, 0.0, 1.0);
}
