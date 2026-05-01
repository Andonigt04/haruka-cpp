/**
 * @file deferred_geom.frag
 * @brief GBuffer fill pass for deferred rendering.
 *
 * Writes per-fragment surface data into 4 render targets:
 *   gPosition  (RGB = world-space position)
 *   gNormal    (RGB = normalized world-space normal)
 *   gAlbedoSpec (RGB = albedo, A = specular intensity; fallbacks if textures are black)
 *   gEmissive  (RGB = emissive color)
 *
 * In:  FragPos, Normal, TexCoord (from deferred_geom.vert)
 * Samplers: texture_diffuse1, texture_specular1, texture_emissive1
 */

#version 450 core

layout(location = 0) out vec4 gPosition;
layout(location = 1) out vec4 gNormal;
layout(location = 2) out vec4 gAlbedoSpec;
layout(location = 3) out vec4 gEmissive;
layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;

layout(set = 0, binding = 0) uniform sampler2D texture_diffuse1;
layout(set = 0, binding = 1) uniform sampler2D texture_specular1;
layout(set = 0, binding = 2) uniform sampler2D texture_emissive1;

void main()
{
    gPosition = vec4(FragPos, 1.0);
    gNormal = vec4(normalize(Normal), 1.0);
    vec3 albedo = texture(texture_diffuse1, TexCoord).rgb;
    if(length(albedo) < 0.01) albedo = vec3(0.7);
    vec4 specTex = texture(texture_specular1, TexCoord);
    float spec = (length(specTex.rgb) > 0.01) ? specTex.r : 0.5;
    gAlbedoSpec = vec4(albedo, spec);
    vec3 emissive = texture(texture_emissive1, TexCoord).rgb;
    gEmissive = vec4(emissive, 1.0);
}