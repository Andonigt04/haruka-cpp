/**
 * @file bloom_blur.frag
 * @brief Separable 5-tap Gaussian blur for the bloom pipeline.
 *
 * Applied twice per bloom iteration — once horizontally, once vertically —
 * to approximate a 2D Gaussian with fixed weights:
 *   [0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216]
 *
 * In:  TexCoords
 * Out: FragColor (blurred image)
 * Sampler: image (source HDR buffer)
 * UBO: Params { horizontal } — selects blur axis
 */
#version 450 core

layout(location = 0) out vec4 FragColor;
layout(location = 0) in vec2 TexCoords;

layout(set = 0, binding = 0) uniform sampler2D image;
layout(set = 0, binding = 1) uniform Params {
    bool horizontal;
};

const float weight[5] = float[] (0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

void main()
{
    vec2 tex_offset = 1.0 / textureSize(image, 0);
    vec3 result = texture(image, TexCoords).rgb * weight[0];
    if(horizontal)
    {
        for(int i = 1; i < 5; ++i)
        {
            result += texture(image, TexCoords + vec2(tex_offset.x * i, 0.0)).rgb * weight[i];
            result += texture(image, TexCoords - vec2(tex_offset.x * i, 0.0)).rgb * weight[i];
        }
    }
    else
    {
        for(int i = 1; i < 5; ++i)
        {
            result += texture(image, TexCoords + vec2(0.0, tex_offset.y * i)).rgb * weight[i];
            result += texture(image, TexCoords - vec2(0.0, tex_offset.y * i)).rgb * weight[i];
        }
    }
    FragColor = vec4(result, 1.0);
}