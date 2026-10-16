/**
 * @file softbody.vert
 * @brief XPBD softbody vertex shader. Positions are already camera-relative
 *        (uploaded per frame by SoftBodyRenderer), so only view-rotation +
 *        projection are applied — same convention as planet/water.
 */
#version 450 core
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec3 aPos;     // camera-relative position
layout(location = 1) in vec3 aNormal;
layout(location = 2) in float aAlpha;  // per-vertex opacity (water depth fade); 0 when attrib unbound

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;
layout(location = 2) out float Alpha;

#include "lib/frame_ubo.glsl"   // PerFrameData (binding 0)

void main() {
    FragPos = aPos;
    Normal  = aNormal;
    Alpha   = aAlpha;
    gl_Position = projection * mat4(mat3(view)) * vec4(aPos, 1.0);
}
