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
 * Sampler: image (source HDR buffer), unidad 0
 * UBO: BloomParams (binding 2) — horizontal (1 = eje X, 0 = eje Y). Antes era un uniform suelto
 *      (`layout(location=0) uniform bool`); los glUniform* NO existen en Vulkan → va por UBO
 *      (ruta PSO/RHI). Mismo bloque que bloom_extract.frag. Se usa float (no bool): un bool en
 *      std140 ocupa 4 B y su empaquetado es una fuente clásica de bugs entre GL y Vulkan.
 */
#version 450 core

layout(location = 0) out vec4 FragColor;
layout(location = 0) in vec2 TexCoords;

layout(binding = 0) uniform sampler2D image;

layout(std140, binding = 2) uniform BloomParams {
    float threshold;    // bright-pass (no lo usa este pase)
    float horizontal;   // 1 = horizontal, 0 = vertical
};

const float weight[5] = float[] (0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

void main()
{
    vec2 tex_offset = 1.0 / textureSize(image, 0);
    vec3 result = texture(image, TexCoords).rgb * weight[0];
    if(horizontal > 0.5)
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