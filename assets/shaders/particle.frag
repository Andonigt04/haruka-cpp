/**
 * @file particle.frag
 * @brief Partícula redonda (recorta el cuadrado del punto a un círculo) con color+alpha.
 */
#version 450 core

layout(location = 0) in vec3  vColor;
layout(location = 1) in float vAlpha;

layout(location = 0) out vec4 FragColor;

void main() {
    vec2 c = gl_PointCoord - vec2(0.5);
    float d = length(c);
    if (d > 0.5) discard;                       // círculo
    float core = smoothstep(0.5, 0.0, d);       // brillante en el centro
    vec3  col  = vColor * (0.7 + core * core * 2.2); // NÚCLEO radiante
    float a    = vAlpha * smoothstep(0.5, 0.12, d);  // borde definido
    FragColor = vec4(col, a);
}
