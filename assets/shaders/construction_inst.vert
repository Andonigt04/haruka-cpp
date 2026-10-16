/**
 * @file construction_inst.vert
 * @brief Vertex shader INSTANCIADO para piezas de construcción. Como simple.vert pero el `model` y el
 *        color vienen del STREAM DE INSTANCIA (binding 1), no del UBO per-objeto → un draw por modelo.
 *
 * In (per-vertex, binding 0):  aPos (loc 0), aNormal (loc 1), aUv (loc 2)  [subconjunto del Vertex de escena]
 * In (per-instance, binding 1): ver lib/instance.glsl (pos 3, giro 4, escala 5, tinte 7)
 * Out: Normal (0), FragPos (1), InstanceColor (2), TexCoord (3) → construction_inst.frag
 * UBO: PerFrameData (binding 0) — MISMO layout que simple.vert/final.frag.
 */
#version 450 core
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;

// Stream de instancia (divisor=1): posición + giro + escala + tinte por pieza (40 B).
#include "lib/instance.glsl"

#include "lib/frame_ubo.glsl"   // PerFrameData (binding 0)

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;
layout(location = 2) out vec4 InstanceColor;
layout(location = 3) out vec2 TexCoord;

void main() {
    const vec4 q = harukaInstanceQuat();
    vec3 worldPos = harukaInstancePoint(q, aPos);
    FragPos = worldPos;
    Normal  = harukaInstanceNormal(q, aNormal);   // R·(n/s): sin inverse() por vértice
    InstanceColor = vec4(harukaInstanceTint(), 1.0);
    TexCoord = aUv;
    gl_Position = projection * view * vec4(worldPos, 1.0);
}
