/**
 * @file equirect_to_cubemap.frag
 * @brief Converts an equirectangular HDR panorama to a cubemap face.
 *
 * For each fragment the cube-local WorldPos is normalized and converted to
 * spherical (phi, theta) coordinates via atan2/asin, then remapped to [0,1]
 * UV to sample the equirectangular texture.
 *
 * In:  WorldPos (cube vertex in local space)
 * Out: FragColor (cubemap face color)
 * Sampler: equirectangularMap (2D equirectangular HDR)
 */
#version 450 core

layout(location = 0) out vec4 FragColor;
layout(location = 0) in vec3 WorldPos;

layout(set = 0, binding = 0) uniform sampler2D equirectangularMap;

const vec2 invAtan = vec2(0.1591, 0.3183);
vec2 SampleSphericalMap(vec3 v)
{
    vec2 uv = vec2(atan(v.z, v.x), asin(v.y));
    uv *= invAtan;
    uv += 0.5;
    return uv;
}

void main()
{
    vec2 uv = SampleSphericalMap(normalize(WorldPos));
    vec3 color = texture(equirectangularMap, uv).rgb;
    FragColor = vec4(color, 1.0);
}