/**
 * @file ssao.frag
 * @brief Screen-Space Ambient Occlusion.
 *
 * Builds a TBN matrix from a random rotation vector (tiled 4×noise texture)
 * and samples `kernelSize` hemisphere directions (max 64) rotated into
 * view space. A rangeCheck smoothstep prevents contributions from samples
 * too far in depth (avoids halos). Output is a single float [0,1]:
 *   1.0 = fully unoccluded,  0.0 = fully occluded.
 *
 * In:  TexCoords (screen UV)
 * Out: FragColor (float occlusion factor)
 * Samplers: gPosition (binding 2), gNormal (binding 3), texNoise (binding 4)
 * UBO: Params { samples[64], kernelSize, radius, bias }, Matrices { projection }
 */
#version 450 core

layout(location = 0) out float FragColor;

layout(location = 0) in vec2 TexCoords;

layout(set = 0, binding = 2) uniform sampler2D gPosition;
layout(set = 0, binding = 3) uniform sampler2D gNormal;
layout(set = 0, binding = 4) uniform sampler2D texNoise;

layout(set = 0, binding = 0) uniform Params {
    vec3 samples[64];
    int kernelSize;
    float radius;
    float bias;
} params;

layout(set = 0, binding = 1) uniform Matrices {
    mat4 projection;
};

void main()
{
    vec3 fragPos = texture(gPosition, TexCoords).rgb;
    vec3 normal = normalize(texture(gNormal, TexCoords).rgb);
    vec3 randomVec = normalize(texture(texNoise, TexCoords * 4.0).rgb);
    
    vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 TBN = mat3(tangent, bitangent, normal);
    
    float occlusion = 0.0;
    for(int i = 0; i < params.kernelSize; ++i)
    {
        vec3 samplePos = fragPos + TBN * params.samples[i] * params.radius;
        
        vec4 offset = vec4(samplePos, 1.0);
        offset = projection * offset;
        offset.xyz /= offset.w;
        offset.xyz = offset.xyz * 0.5 + 0.5;
        
        float sampleDepth = texture(gPosition, offset.xy).z;
        float rangeCheck = smoothstep(0.0, 1.0, params.radius / abs(fragPos.z - sampleDepth));
        occlusion += (sampleDepth >= samplePos.z + params.bias ? 1.0 : 0.0) * rangeCheck;
    }
    
    occlusion = 1.0 - (occlusion / float(params.kernelSize));
    FragColor = occlusion;
}