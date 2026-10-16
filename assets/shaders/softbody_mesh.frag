/**
 * @file softbody_mesh.frag
 * @brief La piel de autor: color base = textura (si hay) x color por vértice x tinte, con la misma luz
 *        que `softbody.frag` y que los props: `harukaToonShade` (low-poly sin costuras), de dos caras.
 */
#version 450 core
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;
layout(location = 2) in vec2 UV;
layout(location = 3) in vec3 Color;

layout(std140, binding = 6) uniform SoftbodyMeshParams {
    vec3  u_tint;
    float u_hasTexture;   // 1 = muestrear uBaseColor; 0 = sólo color por vértice
};
layout(binding = 1) uniform sampler2D uBaseColor;

#include "lib/toon_shade.glsl"   // + frame_ubo (PerFrameData, binding 0)

void main() {
    vec3 albedo = Color * u_tint;
    // glTF/Blender ponen la V de las UV hacia ABAJO respecto a la imagen; el exportador ya la invierte
    // (ver tools/blender_haruka.py), así que aquí se muestrea tal cual.
    if (u_hasTexture > 0.5) albedo *= texture(uBaseColor, UV).rgb;

    // Dos caras: la normal suave se gira hacia la cámara; la de cara ya se orienta sola hacia `V`.
    vec3 V = normalize(-FragPos);
    vec3 smoothN = normalize(Normal);
    if (dot(smoothN, V) < 0.0) smoothN = -smoothN;
    vec3 L = normalize(sunDirection);

    HarukaToonParams tp = harukaToonDefaults();
    vec3 N = harukaLightingNormal(smoothN, FragPos, V, tp.facet);
    // Sin material PBR: dieléctrico de rugosidad media (el brillo de 48 que tenía) y sin oclusión.
    vec3 color = harukaToonShade(albedo, N, smoothN, L, V, 0.0, 0.55, 1.0, tp);
    FragColor = vec4(harukaOutputTonemap(color), 1.0);
}
