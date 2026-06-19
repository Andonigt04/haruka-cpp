/**
 * @file skinned.frag
 * @brief Modelos animados: lambert simple + niebla (placeholder gris hasta meter
 *        texturas/material). Comparte u_fogEnabled con el resto.
 */
#version 450 core

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;
layout(location = 2) in vec2 TexCoord;

layout(location = 0) out vec4 FragColor;

layout(location = 15) uniform int u_fogEnabled;

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 _cameraPos;      float _pad0;
    vec3 sunDirection;    float _pad1;
    vec3 sunLightColor;   float ambientStrength;
    int  enableHDR;
};

void main() {
    vec3 N = normalize(Normal);
    float diff = max(dot(N, normalize(sunDirection)), 0.0);
    vec3 base = vec3(0.70, 0.70, 0.72); // placeholder (textura/material luego)
    vec3 col = base * (ambientStrength + diff) * sunLightColor;

    const vec3 FOG_COLOR = vec3(0.60, 0.63, 0.68);
    float fog = (u_fogEnabled != 0) ? (1.0 - exp(-length(FragPos) * 0.00035)) : 0.0;
    col = mix(col, FOG_COLOR, fog);

    FragColor = vec4(col, 1.0);
}
