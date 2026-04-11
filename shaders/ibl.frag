#version 460 core
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D gPosition;
uniform sampler2D gNormal;
uniform sampler2D gAlbedoSpec;
uniform samplerCube irradianceMap;
uniform samplerCube prefilterMap;
uniform sampler2D brdfLUT;

uniform vec3 viewPos;
uniform samplerCube envMap;

const float PI = 3.14159265359;

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