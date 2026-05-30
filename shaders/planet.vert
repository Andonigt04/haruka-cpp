/**
 * @file planet.vert
 * @brief Vertex shader for planetary terrain chunks.
 *
 * Vertices are stored relative to their chunk center (float-safe).
 * u_chunkOffset = chunkCenter - cameraPos, supplied per draw call.
 *
 * CDLOD geomorphing: aMorphTarget is each vertex's position in the decimated
 * (parent-LOD) mesh. u_morphFactor is computed PER-CHUNK on the CPU from the
 * chunk-CENTER distance (height-independent), so the whole chunk morphs
 * coherently. Per-vertex morphing using the displaced position tears tall
 * terrain into stair-steps (a peak is kilometres nearer than its valley), so
 * a uniform per-chunk factor is used instead.
 *
 * In:  aPos (0), aNormal (1), aUV (2), aMorphTarget (3)
 */
#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aMorphTarget;

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;
layout(location = 2) out vec2 TexCoord;

layout(location = 10) uniform vec3  u_chunkOffset;
layout(location = 12) uniform float u_morphFactor; // 0=full detail, 1=parent shape

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 _cameraPos;      float _pad0;
    vec3 sunDirection;    float _pad1;
    vec3 sunLightColor;   float ambientStrength;
    int  enableHDR;
};

void main() {
    vec3 morphedPos = mix(aPos, aMorphTarget, u_morphFactor);
    vec3 camRelPos  = u_chunkOffset + morphedPos;
    FragPos  = camRelPos;
    Normal   = normalize(aNormal);
    TexCoord = aUV;
    gl_Position = projection * mat4(mat3(view)) * vec4(camRelPos, 1.0);
}
