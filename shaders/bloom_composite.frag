/**
 * @file bloom_composite.frag
 * @brief Composites the scene with the blurred bloom texture.
 *
 * Adds bloom on top of the scene color scaled by bloomStrength, then applies
 * Reinhard tone mapping and gamma correction (γ = 2.2) in a single pass.
 *
 * In:  TexCoords
 * Out: FragColor (LDR, gamma-corrected)
 * Samplers: scene (binding 0), bloom (binding 1)
 * Uniforms: bloomStrength (location 0)
 */

#version 450 core

layout(location = 0) out vec4 FragColor;
layout(location = 0) in vec2 TexCoords;

layout(binding = 0) uniform sampler2D scene;
layout(binding = 1) uniform sampler2D bloom;
layout(location = 0) uniform float bloomStrength;

void main()
{
    vec3 sceneColor = texture(scene, TexCoords).rgb;
    vec3 bloomColor = texture(bloom, TexCoords).rgb;
    vec3 result = sceneColor + bloomColor * bloomStrength;
    result = result / (result + vec3(1.0));
    result = pow(result, vec3(1.0/2.2));
    FragColor = vec4(result, 1.0);
}