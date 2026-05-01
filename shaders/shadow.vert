/**
 * @file shadow.vert
 * @brief Directional shadow-map depth pass vertex shader.
 *
 * Projects each vertex directly to light-clip space so the depth buffer
 * captures the scene depth from the light's point of view.
 * Paired with shadow.frag (empty body — depth is written by fixed-function).
 *
 * In:  aPos
 * Uniforms: model (location 0), lightSpaceMatrix (location 4)
 */
#version 450 core

layout (location = 0) in vec3 aPos;

layout(location = 0) uniform mat4 model;             // locations 0–3
layout(location = 4) uniform mat4 lightSpaceMatrix;  // locations 4–7

void main()
{
    gl_Position = lightSpaceMatrix * model * vec4(aPos, 1.0);
}