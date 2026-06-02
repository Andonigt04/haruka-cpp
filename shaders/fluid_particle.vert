/**
 * @file fluid_particle.vert
 * @brief PBF fluid particles as camera-facing point sprites (Fase C).
 * Positions are camera-relative; point size scales with distance so near
 * particles look bigger (perspective). Sphere shading is done in the fragment.
 */
#version 450 core

layout(location = 0) in vec3 aPos; // camera-relative particle position

layout(location = 1) out vec3 vViewPos;

layout(location = 12) uniform float u_radius;     // world particle radius (m)
layout(location = 13) uniform float u_viewportH;  // viewport height (px)

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 _cameraPos;     float _pad0;
    vec3 sunDirection;   float _pad1;
    vec3 sunLightColor;  float ambientStrength;
    int  enableHDR;
};

void main() {
    vec3 vp = mat3(view) * aPos; // view-space (camera at origin)
    vViewPos = vp;
    vec4 clip = projection * vec4(vp, 1.0);
    gl_Position = clip;
    // Project sphere radius to pixels: size = radius * focal / depth.
    float focal = projection[1][1] * 0.5 * u_viewportH;
    float dist = max(0.001, -vp.z);
    gl_PointSize = clamp(2.0 * u_radius * focal / dist, 2.0, 128.0);
}
