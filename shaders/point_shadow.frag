/**
 * @file point_shadow.frag
 * @brief Writes normalized linear depth for the omnidirectional shadow cubemap.
 *
 * Converts the raw distance from fragment to light into a [0, 1] value
 * by dividing by farPlane, then writes it to gl_FragDepth. This avoids
 * the non-linear depth distortion of the default perspective depth buffer,
 * allowing correct distance comparisons in deferred_light.frag.
 *
 * In:  FragPos (world-space position, vec4 from point_shadow.geom)
 * UBO: Params { lightPos, farPlane }
 */
#version 450 core

layout(location = 0) in vec4 FragPos;

layout(location = 28) uniform vec3  lightPos;
layout(location = 29) uniform float farPlane;

void main()
{
    float lightDistance = length(FragPos.xyz - lightPos);
    lightDistance = lightDistance / farPlane;
    gl_FragDepth = lightDistance;
}