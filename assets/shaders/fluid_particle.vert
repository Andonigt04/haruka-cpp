/**
 * @file fluid_particle.vert
 * @brief PBF fluid particles as camera-facing point sprites (Fase C).
 * Positions are camera-relative; point size scales with distance so near
 * particles look bigger (perspective). Sphere shading is done in the fragment.
 */
#version 450 core

layout(location = 0) in vec3 aPos; // camera-relative particle position

layout(location = 1) out vec3 vViewPos;

// Parámetros del pase de fluido. Antes eran uniforms sueltos (glUniform no existe en Vulkan).
// El bloque es IDÉNTICO en los 4 shaders del fluido (comparten el binding 8); cada uno usa lo suyo.
layout(std140, binding = 8) uniform FluidParams {
    vec2  u_blurDir;        // blur separable: (1/w,0) o (0,1/h)
    vec2  u_texel;          // 1/resolución
    float u_depthFalloff;   // m; menor = bordes más nítidos
    float u_refractScale;   // refracción en unidades uv
    float u_radius;         // radio de partícula (m)
    float u_viewportH;      // alto del viewport (px)
};

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
