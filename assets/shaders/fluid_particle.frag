/**
 * @file fluid_particle.frag
 * @brief Shades point sprites as lit spheres (Fase C). Discards fragments
 * outside the disc and reconstructs a sphere normal for cheap shading. This is
 * the validation look; screen-space fluid surfacing comes later if desired.
 */
#version 450 core

layout(location = 1) in vec3 vViewPos;
layout(location = 0) out vec4 FragColor;

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 _cameraPos;     float _pad0;
    vec3 sunDirection;   float _pad1;
    vec3 sunLightColor;  float ambientStrength;
    int  enableHDR;
};

void main() {
    // gl_PointCoord in [0,1]; map to [-1,1] disc.
    vec2 d = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(d, d);
    if (r2 > 1.0) discard;                 // round sprite
    float z = sqrt(1.0 - r2);
    vec3 N = vec3(d.x, -d.y, z);           // view-space sphere normal

    // Sun direction into view space.
    vec3 Lv = normalize(mat3(view) * sunDirection);
    float ndl = max(dot(N, Lv), 0.0);

    vec3 base = vec3(0.10, 0.40, 0.70);
    vec3 V = vec3(0,0,1);
    vec3 H = normalize(Lv + V);
    float spec = pow(max(dot(N,H),0.0), 64.0);

    vec3 color = ambientStrength * base
               + 0.85 * ndl * sunLightColor * base
               + sunLightColor * 0.4 * spec;

    if (enableHDR != 0) { color = color/(color+vec3(1.0)); color = pow(color, vec3(1.0/2.2)); }
    else color = clamp(color, 0.0, 1.0);

    FragColor = vec4(color, 1.0);
}
