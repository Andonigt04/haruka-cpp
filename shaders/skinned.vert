/**
 * @file skinned.vert
 * @brief GPU skinning. Transforma cada vértice por sus huesos (hasta 4) usando las
 *        matrices finales del Animator (UBO binding 4). Render camera-relativo:
 *        u_model lleva la traslación = worldPos - cámara.
 * In: aPos(0) aNormal(1) aUV(2) aBoneIDs(3, ivec4) aWeights(4)
 */
#version 450 core

layout(location = 0) in vec3  aPos;
layout(location = 1) in vec3  aNormal;
layout(location = 2) in vec2  aUV;
layout(location = 3) in ivec4 aBoneIDs;
layout(location = 4) in vec4  aWeights;

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;
layout(location = 2) out vec2 TexCoord;

layout(location = 10) uniform mat4 u_model; // camera-relative

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 _cameraPos;      float _pad0;
    vec3 sunDirection;    float _pad1;
    vec3 sunLightColor;   float ambientStrength;
    int  enableHDR;
};

const int MAX_BONES = 100;
layout(std140, binding = 4) uniform BoneMatrices { mat4 u_finalBones[MAX_BONES]; };

void main() {
    vec4 pos = vec4(0.0);
    vec3 nrm = vec3(0.0);
    float wsum = 0.0;
    for (int i = 0; i < 4; i++) {
        if (aBoneIDs[i] < 0) continue;
        if (aBoneIDs[i] >= MAX_BONES) { pos = vec4(aPos, 1.0); nrm = aNormal; wsum = 1.0; break; }
        mat4 bm = u_finalBones[aBoneIDs[i]];
        pos += (bm * vec4(aPos, 1.0)) * aWeights[i];
        nrm += mat3(bm) * aNormal * aWeights[i];
        wsum += aWeights[i];
    }
    if (wsum < 0.0001) { pos = vec4(aPos, 1.0); nrm = aNormal; } // sin rig → estático

    vec4 worldRel = u_model * vec4(pos.xyz, 1.0);
    FragPos  = worldRel.xyz;
    Normal   = normalize(mat3(u_model) * nrm);
    TexCoord = aUV;
    gl_Position = projection * mat4(mat3(view)) * worldRel;
}
