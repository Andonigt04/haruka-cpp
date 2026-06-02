/**
 * @file softbody.vert
 * @brief XPBD softbody vertex shader. Positions are already camera-relative
 *        (uploaded per frame by SoftBodyRenderer), so only view-rotation +
 *        projection are applied — same convention as planet/water.
 */
#version 450 core

layout(location = 0) in vec3 aPos;     // camera-relative position
layout(location = 1) in vec3 aNormal;

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;

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
    gl_Position = projection * mat4(mat3(view)) * vec4(aPos, 1.0);
}
