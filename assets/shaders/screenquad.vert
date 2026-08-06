/**
 * @file screenquad.vert
 * @brief Full-screen quad vertex shader for post-processing passes.
 *
 * Positions are in NDC; no transform needed. Used by every pass that
 * reads from a texture and writes to a framebuffer: SSAO, blur, bloom
 * extract, composite, tonemapping, etc.
 *
 * In:  aPos (NDC xy), aTexCoords
 * Out: TexCoords
 */
#version 450 core

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoords;
layout(location = 0) out vec2 TexCoords;

void main()
{
    gl_Position = vec4(aPos, 0.0, 1.0);
    TexCoords = aTexCoords;
}