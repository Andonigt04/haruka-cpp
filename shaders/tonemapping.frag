/**
 * @file tonemapping.frag
 * @brief Exposure-based tone mapping with bloom composite and gamma correction.
 *
 * Applies exponential (Reinhard-like) tone mapping:
 *   mapped = 1 - exp(-hdrColor * exposure)
 * Then adds the bloom contribution scaled by bloomStrength, and applies
 * gamma correction (γ = 2.2). Final output is LDR sRGB.
 *
 * In:  TexCoords (screen UV)
 * Out: FragColor (LDR, gamma-corrected)
 * Samplers: scene (HDR, binding 1), bloom (blurred bright regions, binding 2)
 * UBO: Params { exposure, bloomStrength }
 */
#version 450 core

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec2 TexCoords;

layout(set = 0, binding = 1) uniform sampler2D scene;
layout(set = 0, binding = 2) uniform sampler2D bloom;

layout(set = 0, binding = 0) uniform Params {
    float exposure;
    float bloomStrength;
} params;

void main()
{
    vec3 hdrColor = texture(scene, TexCoords).rgb;
    vec3 bloomColor = texture(bloom, TexCoords).rgb;
    
    // Tone mapping
    vec3 mapped = vec3(1.0) - exp(-hdrColor * params.exposure);

    // Bloom
    mapped += bloomColor * params.bloomStrength;

    // Gamma correction
    mapped = pow(mapped, vec3(1.0 / 2.2));

    FragColor = vec4(mapped, 1.0);
}