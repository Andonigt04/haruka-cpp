/**
 * @file preview.vert
 * @brief Preview-mode vertex shader (Simple visualization).
 *
 * Identical interface to simple.vert; uses the same UBO bindings so
 * both can be swapped without changing C++ UBO upload code.
 *
 * In:  aPos (loc 0), aNormal (loc 1)
 * Out: Normal (loc 0), FragPos (loc 1)
 * UBOs: PerFrameData (binding 0), PerObjectData (binding 1)
 */
#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 cameraPos;      float _pad0;
    vec3 sunDirection;   float _pad1;
    vec3 sunLightColor;  float ambientStrength;
    int  enableHDR;
    int  enableBloom;
    int  enableSSAO;
    int  enableIBL;
    int  enableShadows;
    int  _pad3a; int _pad3b; int _pad3c; // 3 ints sueltos (no array std140 → moon alineada a 208)
};

layout(std140, binding = 1) uniform PerObjectData {
    mat4 model;
    vec4 baseColorAndPlanetRadius;
    vec4 planetCenterAndFlag;
};

void main() {
    vec4 worldPos = model * vec4(aPos, 1.0);
    FragPos = worldPos.xyz;

    mat3 normalMatrix = mat3(transpose(inverse(model)));
    Normal = normalize(normalMatrix * aNormal);

    gl_Position = projection * view * worldPos;
}
