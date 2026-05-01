/**
 * @file shadow.vert
 * @brief Directional shadow-map depth pass vertex shader.
 *
 * Projects each vertex directly to light-clip space so the depth buffer
 * captures the scene depth from the light's point of view.
 * Paired with shadow.frag (empty body — depth is written by fixed-function).
 *
 * In:  aPos
 * UBO: Matrices { model, lightSpaceMatrix }
 */
#version 450 core

layout (location = 0) in vec3 aPos;

layout(set = 0, binding = 0) uniform Matrices {
    mat4 model;
    mat4 lightSpaceMatrix;
};

void main()
{
    gl_Position = lightSpaceMatrix * model * vec4(aPos, 1.0);
}