/**
 * @file item_preview.frag
 * @brief Sombreado del icono de inventario. Ver item_preview.vert.
 *
 * Suave y neutro a propósito: la casilla debe LEERSE de un vistazo, no ser una escena. Luz fija de
 * tres cuartos, sin sombras ni tonemap — el icono no comparte iluminación con el mundo.
 */
#version 450 core

layout(location = 0) in vec3 vN;
layout(location = 1) in vec3 vC;
layout(location = 0) out vec4 FragColor;

void main() {
    vec3 N = normalize(vN);
    vec3 L = normalize(vec3(0.4, 0.8, 0.5));
    float d = max(dot(N, L), 0.0);
    vec3 c = vC * (0.45 + 0.65 * d);
    c += vec3(0.10) * pow(1.0 - max(dot(N, vec3(0, 0, 1)), 0.0), 2.0);   // borde suave
    FragColor = vec4(c, 1.0);
}
