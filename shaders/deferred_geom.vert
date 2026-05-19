/**
 * @file deferred_geom.vert
 * @brief Geometry-pass vertex shader for deferred rendering.
 *
 * Transforms each vertex into world space (FragPos, Normal) and clip space.
 * Normal is corrected for non-uniform scaling via the inverse-transpose of the
 * model matrix.
 *
 * In:  aPos, aNormal, aTexCoord
 * Out: FragPos (world), Normal (world), TexCoord → deferred_geom.frag
 * UBO: Matrices { model, view, projection }
 */
#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec3 Normal;
layout(location = 2) out vec2 TexCoord;

layout(location = 0) uniform mat4 model;       // locations 0-3
layout(location = 4) uniform mat4 view;        // locations 4-7
layout(location = 8) uniform mat4 projection;  // locations 8-11

void main()
{
    FragPos = vec3(model * vec4(aPos, 1.0));
    Normal = mat3(transpose(inverse(model))) * aNormal;
    TexCoord = aTexCoord;
    gl_Position = projection * view * vec4(FragPos, 1.0);
}