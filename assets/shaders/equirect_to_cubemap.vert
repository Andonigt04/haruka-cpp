/**
 * @file equirect_to_cubemap.vert
 * @brief Cube-face vertex shader for equirectangular-to-cubemap conversion.
 *
 * Passes the local cube vertex position as WorldPos so the fragment shader
 * can map it to spherical coordinates and sample the equirectangular texture.
 * Rendered 6 times with different view matrices to fill each cubemap face.
 *
 * In:  aPos (cube vertex in local space)
 * Out: WorldPos → equirect_to_cubemap.frag
 * UBO: Matrices { projection, view }
 */
#version 450 core

layout (location = 0) in vec3 aPos;
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