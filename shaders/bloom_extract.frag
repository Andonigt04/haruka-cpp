/**
 * @file bloom_extract.frag
 * @brief Extracts bright pixels for the bloom pipeline.
 *
 * Computes perceptual luminance using BT.709 coefficients
 * (0.2126 R + 0.7152 G + 0.0722 B). Pixels above `threshold` pass through;
 * others are zeroed. The result is fed into bloom_blur.frag.
 *
 * In:  TexCoords
 * Out: FragColor — original color if bright, black otherwise
 * Sampler: scene (HDR render target)
 * UBO: Params { threshold }
 */
#version 450 core

layout(location = 0) out vec4 FragColor;
layout(location = 0) in vec2 TexCoords;

layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 0, binding = 1) uniform Params {
    float threshold;
};

void main()
{
    vec3 color = texture(scene, TexCoords).rgb;
    float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
    if(brightness > threshold)
        FragColor = vec4(color, 1.0);
    else
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
}