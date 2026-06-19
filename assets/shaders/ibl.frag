/**
 * @file ibl.frag
 * @brief Image-Based Lighting ambient contribution pass.
 *
 * Reads the GBuffer and computes the diffuse IBL ambient term using the
 * Fresnel-Schlick roughness approximation. Metallic surfaces contribute less
 * diffuse (kD suppressed). Result is blended additively over deferred lighting.
 *
 * In:  TexCoords (screen UV)
 * Out: FragColor (HDR ambient contribution)
 * Samplers: gPosition, gNormal, gAlbedoSpec (metallic in .a),
 *           irradianceMap, prefilterMap, brdfLUT, envMap
 * UBO: Params { viewPos }
 */
#version 450 core

layout(location = 0) out vec4 FragColor;
layout(location = 0) in vec2 TexCoords;

layout(set = 0, binding = 0) uniform sampler2D gPosition;
layout(set = 0, binding = 1) uniform sampler2D gNormal;
layout(set = 0, binding = 2) uniform sampler2D gAlbedoSpec;
layout(set = 0, binding = 3) uniform samplerCube irradianceMap;
layout(set = 0, binding = 4) uniform samplerCube prefilterMap; // reserved — specular IBL not yet implemented
layout(set = 0, binding = 5) uniform sampler2D brdfLUT;        // reserved — specular IBL not yet implemented
layout(set = 0, binding = 6) uniform samplerCube envMap;       // reserved — specular IBL not yet implemented

layout(set = 0, binding = 7) uniform Params {
    vec3 viewPos;
};

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main()
{
    vec3 FragPos = texture(gPosition, TexCoords).rgb;
    vec3 Normal  = normalize(texture(gNormal, TexCoords).rgb);
    vec3 Albedo  = texture(gAlbedoSpec, TexCoords).rgb;
    float Metallic = texture(gAlbedoSpec, TexCoords).a;

    vec3 V = normalize(viewPos - FragPos);
    
    // Ambient IBL
    vec3 F0 = mix(vec3(0.04), Albedo, Metallic);
    vec3 F = fresnelSchlickRoughness(max(dot(Normal, V), 0.0), F0, 0.5);
    
    vec3 kS = F;
    vec3 kD = (1.0 - kS) * (1.0 - Metallic);
    
    vec3 irradiance = texture(irradianceMap, Normal).rgb;
    vec3 diffuse = irradiance * Albedo;
    
    vec3 ambient = (kD * diffuse + kS * 0.03) * 0.5;

    FragColor = vec4(ambient, 1.0);
}