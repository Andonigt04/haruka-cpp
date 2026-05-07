#version 450 core

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;

void main() {
    vec3 n = normalize(Normal);

    // Preview lighting: stable and independent from UBO/textures.
    vec3 lightDir = normalize(vec3(-0.35, -0.80, -0.45));
    float ndl = max(dot(n, -lightDir), 0.0);

    vec3 baseColor = vec3(0.72, 0.74, 0.78);
    vec3 ambient = 0.30 * baseColor;
    vec3 diffuse = 0.70 * ndl * baseColor;

    FragColor = vec4(ambient + diffuse, 1.0);
}
