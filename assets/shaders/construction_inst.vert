/**
 * @file construction_inst.vert
 * @brief Vertex shader INSTANCIADO para piezas de construcción. Como simple.vert pero el `model` y el
 *        color vienen del STREAM DE INSTANCIA (binding 1), no del UBO per-objeto → un draw por modelo.
 *
 * In (per-vertex, binding 0):  aPos (loc 0), aNormal (loc 1), aUv (loc 2)  [subconjunto del Vertex de escena]
 * In (per-instance, binding 1): model mat4 (loc 3..6), instanceColor (loc 7), instanceScale (loc 8)
 * Out: Normal (0), FragPos (1), InstanceColor (2), TexCoord (3) → construction_inst.frag
 * UBO: PerFrameData (binding 0) — MISMO layout que simple.vert/final.frag.
 */
#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;

// Stream de instancia (divisor=1): matriz de modelo + color por pieza.
layout(location = 3) in mat4 iModel;       // ocupa loc 3,4,5,6
layout(location = 7) in vec4 iColor;
layout(location = 8) in vec3 iScale;       // no se usa aquí (la escala ya va en iModel); presente por layout

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
    int  _pad3a; int _pad3b; int _pad3c;
    vec3 moonDirection;  float moonIntensity;
    vec3 moonLightColor; float _pad4;
};

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;
layout(location = 2) out vec4 InstanceColor;
layout(location = 3) out vec2 TexCoord;

void main() {
    vec4 worldPos = iModel * vec4(aPos, 1.0);
    FragPos = worldPos.xyz;
    Normal  = normalize(mat3(transpose(inverse(iModel))) * aNormal);
    InstanceColor = iColor;
    TexCoord = aUv;
    gl_Position = projection * view * worldPos;
}
