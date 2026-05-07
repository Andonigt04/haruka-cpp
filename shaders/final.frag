#version 450 core

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;

layout(location = 0) uniform vec3 baseColor;
layout(location = 1) uniform vec3 cameraPos;
layout(location = 2) uniform vec3 sunDirection;
layout(location = 3) uniform bool enableHDR;
layout(location = 4) uniform bool enableBloom;
layout(location = 5) uniform bool enableSSAO;
layout(location = 6) uniform bool enableIBL;
layout(location = 7) uniform bool enableShadows;

void main() {
    vec3 N = normalize(Normal);
    vec3 L = normalize(-sunDirection);
    vec3 V = normalize(cameraPos - FragPos);
    vec3 H = normalize(L + V);

    float ndl = max(dot(N, L), 0.0);
    float spec = pow(max(dot(N, H), 0.0), 48.0);

    vec3 ambient = 0.22 * baseColor;
    vec3 diffuse = 0.63 * ndl * baseColor;
    vec3 highlights = vec3(0.9) * (0.15 * spec);

    if (!enableIBL) {
        ambient *= 0.7;
    }
    if (!enableSSAO) {
        ambient *= 1.05;
    }
    if (!enableShadows) {
        diffuse *= 1.08;
    }
    if (!enableBloom) {
        highlights *= 0.55;
    }

    vec3 color = ambient + diffuse + highlights;
    if (enableHDR) {
        color = color / (color + vec3(1.0));
        color = pow(color, vec3(1.0 / 2.2));
    } else {
        color = clamp(color, 0.0, 1.0);
    }

    FragColor = vec4(color, 1.0);
}
