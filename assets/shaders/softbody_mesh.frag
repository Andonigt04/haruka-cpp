/**
 * @file softbody_mesh.frag
 * @brief La piel de autor: color base = textura (si hay) x color por vértice x tinte, con la misma luz
 *        que `softbody.frag` (Lambert de dos caras + especular del sol).
 */
#version 450 core

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

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 _cameraPos;     float _pad0;
    vec3 sunDirection;   float _pad1;
    vec3 sunLightColor;  float ambientStrength;
    int  enableHDR;
};

void main() {
    vec3 albedo = Color * u_tint;
    // glTF/Blender ponen la V de las UV hacia ABAJO respecto a la imagen; el exportador ya la invierte
    // (ver tools/blender_haruka.py), así que aquí se muestrea tal cual.
    if (u_hasTexture > 0.5) albedo *= texture(uBaseColor, UV).rgb;

    vec3 N = normalize(Normal);
    vec3 V = normalize(-FragPos);
    if (dot(N, V) < 0.0) N = -N;
    vec3 L = normalize(sunDirection);
    vec3 H = normalize(L + V);
    float ndl  = max(dot(N, L), 0.0);
    float spec = pow(max(dot(N, H), 0.0), 48.0);
    vec3 color = ambientStrength * albedo + 0.85 * ndl * sunLightColor * albedo + sunLightColor * 0.15 * spec;
    if (enableHDR != 0) {
        color = color / (color + vec3(1.0));
        color = pow(color, vec3(1.0 / 2.2));
    } else {
        color = clamp(color, 0.0, 1.0);
    }
    FragColor = vec4(color, 1.0);
}
