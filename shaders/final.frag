/**
 * @file final.frag
 * @brief Full-featured forward fragment shader (Final visualization mode).
 *
 * Reads per-frame and per-object data from UBOs instead of explicit-location
 * uniforms, eliminating the location conflict with the vertex shader.
 *
 * In:  Normal (loc 0), FragPos (loc 1)
 * Out: FragColor (loc 0)
 * UBOs: PerFrameData (binding 0), PerObjectData (binding 1)
 */
#version 450 core

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 cameraPos;      float _pad0;
    vec3 sunDirection;   float _pad1;
    vec3 sunLightColor;  float ambientStrength;
    int  enableHDR;
    int  enableBloom;
    int  enableSSAO;
    int  enableIBL;
    int  enableShadows;
    int  _pad3[3];
};

layout(std140, binding = 1) uniform PerObjectData {
    mat4 model;
    vec4 baseColorAndPlanetRadius; // rgb = base color
    vec4 planetCenterAndFlag;
};

void main() {
    vec3 baseColor = baseColorAndPlanetRadius.rgb;

    // Emissive stars: render at full brightness, no shading
    if (planetCenterAndFlag.w > 1.5) {
        FragColor = vec4(baseColor, 1.0);
        return;
    }

    vec3 N = normalize(Normal);
    vec3 L = normalize(sunDirection);
    vec3 V = normalize(cameraPos - FragPos);
    vec3 H = normalize(L + V);

    float ndl  = max(dot(N, L), 0.0);
    float spec = pow(max(dot(N, H), 0.0), 48.0);

    vec3 ambient    = 0.22 * baseColor;
    vec3 diffuse    = 0.63 * ndl * baseColor;
    vec3 highlights = vec3(0.9) * (0.15 * spec);

    if (enableIBL     == 0) ambient    *= 0.7;
    if (enableSSAO    == 0) ambient    *= 1.05;
    if (enableShadows == 0) diffuse    *= 1.08;
    if (enableBloom   == 0) highlights *= 0.55;

    vec3 color = ambient + diffuse + highlights;
    if (enableHDR != 0) {
        color = color / (color + vec3(1.0));
        color = pow(color, vec3(1.0 / 2.2));
    } else {
        color = clamp(color, 0.0, 1.0);
    }

    FragColor = vec4(color, 1.0);
}
