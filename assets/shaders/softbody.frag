/**
 * @file softbody.frag
 * @brief XPBD softbody shading — two-sided Lambert + sun specular.
 */
#version 450 core

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;
layout(location = 2) in float Alpha;

// UBO (binding 6) — antes uniforms sueltos (glUniform3fv/1f), que NO existen en Vulkan.
// std140: vec3 (align 16, size 12) + el float siguiente comparten el slot de 16 B.
layout(std140, binding = 6) uniform SoftbodyParams {
    vec3  u_color;
    float u_alphaMode;   // 1 = usar el alpha por-vértice; 0 = opaco
};
// 0 = opaque (softbodies). >0.5 = use per-vertex Alpha (water depth fade).

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 _cameraPos;     float _pad0;
    vec3 sunDirection;   float _pad1;
    vec3 sunLightColor;  float ambientStrength;
    int  enableHDR;
};

void main() {
    vec3 N = normalize(Normal);
    // Two-sided: flip the normal to face the camera (cloth/jelly viewed from any side).
    vec3 V = normalize(-FragPos);
    if (dot(N, V) < 0.0) N = -N;

    vec3 L = normalize(sunDirection);
    vec3 H = normalize(L + V);

    float ndl  = max(dot(N, L), 0.0);
    float spec = pow(max(dot(N, H), 0.0), 48.0);

    vec3 color = ambientStrength * u_color
               + 0.85 * ndl * sunLightColor * u_color
               + sunLightColor * 0.25 * spec;

    if (enableHDR != 0) {
        color = color / (color + vec3(1.0));
        color = pow(color, vec3(1.0 / 2.2));
    } else {
        color = clamp(color, 0.0, 1.0);
    }
    float a = (u_alphaMode > 0.5) ? Alpha : 1.0;
    FragColor = vec4(color, a);
}
