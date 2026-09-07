/**
 * @file cloud_upsample.vert
 * @brief Triángulo de pantalla completa que sube el pase de nubes a resolución de escena.
 *
 * Sin uniformes a propósito: el pase volumétrico se dibuja en un target REDUCIDO y esto solo lo
 * compone encima. Ver `cloud_upsample.frag` para por qué la interpolación no puede ser la bilineal
 * del sampler.
 */
#version 450 core

layout(location = 0) out vec2 vUV;

void main()
{
    vec2 ndc = vec2((gl_VertexID == 1) ? 3.0 : -1.0,
                    (gl_VertexID == 2) ? 3.0 : -1.0);
    gl_Position = vec4(ndc, 1.0, 1.0);
    vUV = ndc * 0.5 + 0.5;
}
