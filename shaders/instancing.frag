/**
 * @file instancing.frag
 * @brief GBuffer output for GPU-instanced objects.
 *
 * Writes instance color as albedo, reconstructed normal, and world position
 * into the deferred GBuffer so instanced objects participate in IBL, shadows,
 * and SSAO exactly like non-instanced geometry.
 *
 * In:  FragPos, Normal, InstanceColor (from instancing.vert)
 * Out: gPosition, gNormal, gAlbedoSpec, gEmissive
 */
#version 460 core

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 3) in vec4 InstanceColor;

layout(location = 0) out vec3 gPosition;
layout(location = 1) out vec3 gNormal;
layout(location = 2) out vec4 gAlbedoSpec;
layout(location = 3) out vec3 gEmissive;

void main()
{
    gPosition    = FragPos;
    gNormal      = normalize(Normal);
    gAlbedoSpec  = vec4(InstanceColor.rgb, 0.3);
    gEmissive    = vec3(0.0);
}
