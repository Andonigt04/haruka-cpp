/**
 * @file simple.vert
 * @brief Forward-rendering vertex shader.
 *
 * Transforms position to world space (FragPos) and clip space.
 * Normal is corrected for non-uniform scaling via the inverse-transpose
 * of the model matrix.
 * Used with final.frag / light_cube.frag.
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
    int  _pad3[3];
    vec3 moonDirection;  float moonIntensity;
    vec3 moonLightColor; float _pad4;
};

layout(std140, binding = 1) uniform PerObjectData {
    mat4 model;
    vec4 baseColorAndPlanetRadius; // rgb = base color, a = planet radius
    vec4 planetCenterAndFlag;      // xyz = planet center, w = useProceduralTerrain (0/1)
};

void main() {
    vec4 worldPos = model * vec4(aPos, 1.0);
    FragPos = worldPos.xyz;

    mat3 normalMatrix = mat3(transpose(inverse(model)));
    Normal = normalize(normalMatrix * aNormal);

    gl_Position = projection * view * worldPos;
}
