/**
 * @file fluid_depth.frag
 * @brief Screen-space fluid — depth pass. Renders each PBF particle as a sphere
 *        point sprite and writes its FRONT-surface eye-space depth (positive
 *        metres) to an R32F target, plus gl_FragDepth so the nearest particle
 *        wins. The smooth + surface passes turn this into a continuous surface
 *        (instead of the per-particle "balls" look).
 */
#version 450 core

layout(location = 1) in vec3 vViewPos;   // particle CENTRE in view space
layout(location = 0) out float FragEyeDepth;

layout(location = 12) uniform float u_radius; // world particle radius (m)

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 _cameraPos;     float _pad0;
    vec3 sunDirection;   float _pad1;
    vec3 sunLightColor;  float ambientStrength;
    int  enableHDR;
};

void main() {
    vec2 d = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(d, d);
    if (r2 > 1.0) discard;                 // round sprite
    float z = sqrt(1.0 - r2);              // sphere height toward camera [0,1]

    // Front-surface point in view space (camera looks down -z, so +z is closer).
    vec3 frontView = vViewPos + vec3(d.x, -d.y, z) * u_radius;

    // gl_FragDepth so the depth test keeps the nearest particle.
    vec4 clip = projection * vec4(frontView, 1.0);
    gl_FragDepth = (clip.z / clip.w) * 0.5 + 0.5;

    // Eye-space linear depth = distance in front of the camera (positive).
    FragEyeDepth = -frontView.z;
}
