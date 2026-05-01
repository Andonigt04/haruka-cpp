/**
 * @file simple.vert
 * @brief Forward-rendering vertex shader.
 *
 * Transforms position to world space (FragPos) and clip space.
 * Normal is corrected for non-uniform scaling via the inverse-transpose
 * of the model matrix.
 * Used with simple.frag (Blinn-Phong with PCF shadow + normal mapping).
 *
 * In:  aPos, aNormal
 * Out: Normal, FragPos → simple.frag
 * Uniforms (location-bound): model(0), view(1), projection(2)
 */
#version 450 core

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;

layout(location = 0) uniform mat4 model;
layout(location = 1) uniform mat4 view;
layout(location = 2) uniform mat4 projection;

void main() {
    vec4 worldPos = model * vec4(aPos, 1.0);
    FragPos = worldPos.xyz;

    mat3 normalMatrix = mat3(transpose(inverse(model)));
    Normal = normalize(normalMatrix * aNormal);

    gl_Position = projection * view * worldPos;
}
