/**
 * @file ground_stamp.frag
 * @brief La forma de una huella. Ver ground_stamp.vert para el porqué del pase.
 *
 * No es un disco plano: una pisada HUNDE en el centro y LEVANTA un reborde alrededor (el material
 * desplazado tiene que ir a alguna parte). Ese reborde es lo que hace que una huella en nieve se lea
 * como huella y no como una mancha — se ve por su sombra propia, no por su color.
 * Rojo = hundido [0,1] · Verde = reborde levantado [0,1]. El shader del terreno los usa para
 * oscurecer y para inclinar la normal.
 */
#version 450 core

layout(location = 0) in  vec2  vUV;
layout(location = 1) in  float vDepth;
layout(location = 0) out vec4  FragColor;

void main() {
    float r = length(vUV);
    if (r > 1.0) discard;

    float press = (1.0 - smoothstep(0.45, 0.95, r)) * vDepth;          // el hueco
    float rim   = (1.0 - smoothstep(0.16, 0.42, abs(r - 0.82))) * vDepth * 0.8;  // el reborde
    FragColor = vec4(press, rim, 0.0, 1.0);
}
