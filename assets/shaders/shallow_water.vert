/**
 * @file shallow_water.vert
 * @brief Shallow-water heightfield vertex shader. Surface is built per-cell with
 *        a foam attribute (breaking crests). Positions are already camera-relative
 *        (uploaded per frame), so only view-rotation + projection are applied.
 */
#version 450 core

layout(location = 0) in vec3 aPos;     // camera-relative position
layout(location = 1) in vec3 aNormal;  // surface normal (steepness = breaking)
layout(location = 2) in float aAlpha;  // per-vertex opacity (water depth fade)
layout(location = 3) in float aFoam;   // 0..1 breaking-crest foam amount

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;
layout(location = 2) out float Alpha;
layout(location = 3) out float Foam;

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 _cameraPos;     float _pad0;
    vec3 sunDirection;   float _pad1;
    vec3 sunLightColor;  float ambientStrength;
    int  enableHDR;
};

void main() {
    FragPos = aPos;
    Normal  = aNormal;
    Alpha   = aAlpha;
    Foam    = aFoam;
    gl_Position = projection * mat4(mat3(view)) * vec4(aPos, 1.0);
}