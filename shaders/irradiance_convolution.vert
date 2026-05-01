/**
 * @file irradiance_convolution.vert
 * @brief Cube-face vertex shader for irradiance convolution.
 *
 * Identical to equirect_to_cubemap.vert: passes local cube position as
 * WorldPos which the fragment shader uses as the hemisphere normal direction.
 * Rendered 6 times to fill each face of the irradiance cubemap.
 *
 * In:  aPos (cube vertex in local space)
 * Out: WorldPos → irradiance_convolution.frag
 * UBO: Matrices { projection, view }
 */
#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 0) out vec3 WorldPos;

layout(set = 0, binding = 0) uniform Matrices {
    mat4 projection;
    mat4 view;
};

void main()
{
    WorldPos = aPos;
    gl_Position = projection * view * vec4(WorldPos, 1.0);
}