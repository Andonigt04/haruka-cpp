/**
 * @file softbody.frag
 * @brief XPBD softbody shading — two-sided Lambert + sun specular.
 */
#version 450 core
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;
layout(location = 2) in float Alpha;

// UBO (binding 6) — antes uniforms sueltos (glUniform3fv/1f), que NO existen en Vulkan.
// std140: vec3 (align 16, size 12) + el float siguiente comparten el slot de 16 B.
layout(std140, binding = 6) uniform SoftbodyParams {
    vec3  u_color;
    float u_alphaMode;   // 1 = usar el alpha por-vértice; 0 = opaco
};
// 0 = opaque (softbodies). >0.5 = use per-vertex Alpha (water depth fade).

#include "lib/toon_shade.glsl"   // + frame_ubo (PerFrameData, binding 0)

void main() {
    // Two-sided: flip the normal to face the camera (cloth/jelly viewed from any side). La normal de
    // cara (low-poly, lib/lowpoly.glsl) ya se orienta sola hacia `V`.
    vec3 V = normalize(-FragPos);
    vec3 smoothN = normalize(Normal);
    if (dot(smoothN, V) < 0.0) smoothN = -smoothN;
    vec3 L = normalize(sunDirection);

    HarukaToonParams tp = harukaToonDefaults();
    vec3 N = harukaLightingNormal(smoothN, FragPos, V, tp.facet);
    // Gelatina/tela: algo más de brillo que la piel de autor (tenía 0,25 frente a 0,15).
    vec3 color = harukaToonShade(u_color, N, smoothN, L, V, 0.0, 0.40, 1.0, tp);

    float a = (u_alphaMode > 0.5) ? Alpha : 1.0;
    FragColor = vec4(harukaOutputTonemap(color), a);
}
