/**
 * @file point_shadow.vert
 * @brief Vertex shader for the omnidirectional point-shadow depth pass.
 *
 * Transforms each vertex to world space and forwards it as WorldPos to
 * point_shadow.geom, which replicates the triangle to all 6 cubemap faces.
 * gl_Position is set to world-space position (w=1); the geometry shader
 * applies the per-face light-space projection.
 *
 * In:  aPos
 * Out: WorldPos → point_shadow.geom
 * UBO: Matrices { model }
 */
#version 450 core

layout(location = 0) in vec3 aPos;

layout(location = 0) out vec3 WorldPos;

layout(location = 0) uniform mat4 model;

void main()
{
    vec3 worldPos = vec3(model * vec4(aPos, 1.0));
    WorldPos = worldPos;
    gl_Position = vec4(worldPos, 1.0);
}