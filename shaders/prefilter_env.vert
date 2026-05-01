/**
 * @file prefilter_env.vert
 * @brief Cube-face vertex shader for specular environment pre-filtering.
 *
 * Same cube pass-through as equirect_to_cubemap.vert. Rendered 6 times per
 * roughness mip level to build the pre-filtered specular radiance cubemap
 * used by IBL split-sum approximation.
 *
 * In:  aPos (cube vertex in local space)
 * Out: WorldPos → prefilter_env.frag
 * UBO: Matrices { projection, view }
 */
#version 450 core

layout (location = 0) in vec3 aPos;
layout (location = 0) out vec3 WorldPos;

layout(set = 0, binding = 0) uniform Matrices {
    mat4 projection;
    mat4 view;
};

void main()
{
    WorldPos = aPos;
    gl_Position = projection * view * vec4(WorldPos, 1.0);
}