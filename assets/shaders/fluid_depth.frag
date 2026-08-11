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

// Mismo bloque (binding 8) que el resto de shaders del fluido. Ver fluid_particle.vert.
layout(std140, binding = 8) uniform FluidParams {
    vec2  u_blurDir;
    vec2  u_texel;
    float u_depthFalloff;
    float u_refractScale;
    float u_radius;         // radio de partícula (m)
    float u_viewportH;
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
    vec2 d = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(d, d);
    if (r2 > 1.0) discard;                 // round sprite
    float z = sqrt(1.0 - r2);              // sphere height toward camera [0,1]

    // Front-surface point in view space (camera looks down -z, so +z is closer).
    vec3 frontView = vViewPos + vec3(d.x, -d.y, z) * u_radius;

    // gl_FragDepth so the depth test keeps the nearest particle.
    vec4 clip = projection * vec4(frontView, 1.0);
    // ⚠️ SIN el *0.5+0.5 de la convención GL por defecto: el motor usa glClipControl(ZERO_TO_ONE)
    // con reversed-Z (gl_device.cpp:362), así que clip.z/clip.w YA viene en [0,1]. Reescalarlo lo
    // comprimía a [0.5, 1] y el agua quedaba SIEMPRE "más lejos" que el terreno con depth GREATER:
    // fallaba el test y los lagos y ríos no se veían.
    gl_FragDepth = clip.z / clip.w;

    // Eye-space linear depth = distance in front of the camera (positive).
    FragEyeDepth = -frontView.z;
}
