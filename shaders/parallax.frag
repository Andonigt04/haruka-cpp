/**
 * @file parallax.frag
 * @brief PBR (Cook-Torrance) with Parallax Occlusion Mapping.
 *
 * Uses the AO texture channel as a height map to drive steep parallax
 * occlusion mapping (8–32 layers adapted to view angle, with linear
 * interpolation between the last two depth layers). Lighting uses the
 * same Cook-Torrance BRDF as pbr.frag (GGX NDF, Smith-GGX geometry,
 * Schlick Fresnel). Tone mapped via Reinhard + gamma 2.2.
 *
 * In:  TexCoord, Normal, FragPos, Tangent, Bitangent, TBN
 * Out: FragColor (LDR)
 * Samplers: diffuse, normal, metallic_roughness, ao (also height), emissive
 * UBO: Lights[4], Params { viewPos, numLights, heightScale }
 */
#version 450 core

layout(location = 0) in vec2 TexCoord;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec3 FragPos;
layout(location = 3) in vec3 Tangent;
layout(location = 4) in vec3 Bitangent;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D texture_diffuse1;
layout(set = 0, binding = 1) uniform sampler2D texture_normal1;
layout(set = 0, binding = 2) uniform sampler2D texture_metallic_roughness1;
layout(set = 0, binding = 3) uniform sampler2D texture_ao1;
layout(set = 0, binding = 4) uniform sampler2D texture_emissive1;

struct Light {
    vec3 position;
    vec3 color;
};

layout(set = 0, binding = 5) uniform Lights {
    Light lights[4];
};

layout(set = 0, binding = 6) uniform Params {
    vec3 viewPos;
    int numLights;
    float heightScale;
};

const float PI = 3.14159265359;

// Parallax Mapping
vec2 ParallaxMapping(vec2 texCoords, vec3 viewDir)
{
    // Número de capas
    const float minLayers = 8.0;
    const float maxLayers = 32.0;
    float numLayers = mix(maxLayers, minLayers, abs(dot(vec3(0.0, 0.0, 1.0), viewDir)));
    
    float layerDepth = 1.0 / numLayers;
    float currentLayerDepth = 0.0;
    
    vec2 P = viewDir.xy * heightScale;
    vec2 deltaTexCoords = P / numLayers;
    
    vec2 currentTexCoords = texCoords;
    float currentDepthMapValue = texture(texture_ao1, currentTexCoords).r;
    
    while(currentLayerDepth < currentDepthMapValue)
    {
        currentTexCoords -= deltaTexCoords;
        currentDepthMapValue = texture(texture_ao1, currentTexCoords).r;
        currentLayerDepth += layerDepth;
    }
    
    // Interpolación entre dos capas
    vec2 prevTexCoords = currentTexCoords + deltaTexCoords;
    float afterDepth = currentDepthMapValue - currentLayerDepth;
    float beforeDepth = texture(texture_ao1, prevTexCoords).r - currentLayerDepth + layerDepth;
    float weight = afterDepth / (afterDepth - beforeDepth);
    vec2 finalTexCoords = prevTexCoords * weight + currentTexCoords * (1.0 - weight);
    
    return finalTexCoords;
}

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    
    float nom = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    
    return nom / max(denom, 0.0001);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    
    float nom = NdotV;
    float denom = NdotV * (1.0 - k) + k;
    
    return nom / max(denom, 0.0001);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    
    return ggx1 * ggx2;
}

void main()
{
    // Recalcular TBN en fragment shader
    vec3 N = normalize(Normal);
    vec3 T = normalize(Tangent);
    vec3 B = normalize(Bitangent);
    mat3 TBN = mat3(T, B, N);
    
    vec3 viewDir = normalize(viewPos - FragPos);
    vec3 viewDirTangent = normalize(TBN * viewDir);
    
    // Parallax Mapping
    vec2 parallaxTexCoords = ParallaxMapping(TexCoord, viewDirTangent);
    
    // Cargar texturas
    vec4 albedoTex = texture(texture_diffuse1, parallaxTexCoords);
    vec3 albedo = (length(albedoTex.rgb) > 0.01) ? pow(albedoTex.rgb, vec3(2.2)) : vec3(0.5);
    
    vec3 normalMap = texture(texture_normal1, parallaxTexCoords).rgb;
    normalMap = (length(normalMap) > 0.1) ? (normalMap * 2.0 - 1.0) : vec3(0.0, 0.0, 1.0);
    
    vec3 metallicRoughness = texture(texture_metallic_roughness1, parallaxTexCoords).rgb;
    float metallic = (length(metallicRoughness) > 0.01) ? metallicRoughness.b : 0.0;
    float roughness = (length(metallicRoughness) > 0.01) ? metallicRoughness.g : 0.5;
    
    float ao = texture(texture_ao1, parallaxTexCoords).r;
    ao = (ao > 0.01) ? ao : 1.0;
    
    vec3 emissive = texture(texture_emissive1, parallaxTexCoords).rgb;
    
    // Normal - CAMBIAR DE "vec3 N" a "vec3 normalN"
    vec3 normalN = normalize(TBN * normalMap);
    vec3 V = viewDirTangent;

    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);

    vec3 Lo = vec3(0.0);

    for(int i = 0; i < numLights; ++i)
    {
        vec3 L = normalize(TBN * (lights[i].position - FragPos));
        vec3 H = normalize(V + L);
        
        float distance = length(lights[i].position - FragPos);
        float attenuation = 1.0 / (distance * distance + 0.001);
        vec3 radiance = lights[i].color * attenuation;
        
        float NDF = DistributionGGX(normalN, H, roughness);  // CAMBIAR N a normalN
        float G = GeometrySmith(normalN, V, L, roughness);  // CAMBIAR N a normalN
        vec3 F = fresnelSchlick(clamp(dot(H, V), 0.0, 1.0), F0);
        
        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        kD *= 1.0 - metallic;
        
        vec3 numerator = NDF * G * F;
        float denominator = 4.0 * max(dot(normalN, V), 0.0) * max(dot(normalN, L), 0.0) + 0.0001;  // CAMBIAR N a normalN
        vec3 specular = numerator / denominator;
        
        float NdotL = max(dot(normalN, L), 0.0);  // CAMBIAR N a normalN
        Lo += (kD * albedo / PI + specular) * radiance * NdotL;
    }
    
    vec3 ambient = vec3(0.03) * albedo * ao;
    vec3 color = ambient + Lo + emissive;
    
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0/2.2));
    
    FragColor = vec4(color, 1.0);
}