/**
 * @file simple.frag
 * @brief Forward Blinn-Phong shader with PCF shadow, normal mapping, and full light types.
 *
 * Supports three light types in a single pass:
 *   DirLight  — directional with PCF shadow (3×3, 9 samples from shadowMap)
 *   PointLight — up to MAX_POINT_LIGHTS (32) with quadratic attenuation
 *   SpotLight  — cone with inner/outer cutoff (smooth edge via smoothstep)
 * All texture channels have sensible fallbacks (albedo→white, metallic→0,
 * roughness→0.5, ao→1, emissive→black). AO is applied as a post-multiplier.
 *
 * In:  TexCoord, Normal, FragPos, Tangent, Bitangent
 * Out: FragColor
 * Samplers: diffuse, normal, roughness, metallic, metallic_roughness,
 *           ao, emissive, specular, shadowMap
 * UBO: Params, Matrices { lightView, lightProjection },
 *      PointLights[32], DirLightBlock, SpotLightBlock
 */
#version 450 core

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec2 TexCoord;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec3 FragPos;
layout(location = 3) in vec3 Tangent;
layout(location = 4) in vec3 Bitangent;

layout(set = 0, binding = 0) uniform Params {
    vec3 viewPos;
    int nr_active_lights;
    float shininess;
} params;

struct DirLight
{
    vec3 direction;
    vec3 ambient;
    vec3 diffuse;
    vec3 specular;
};

struct PointLight
{
    vec3 position;
    
    float constant;
    float linear;
    float quadratic;
    
    vec3 ambient;
    vec3 diffuse;
    vec3 specular;
};

struct SpotLight
{
    vec3 position;
    vec3 direction;
    float cutOff;
    float outerCutOff;
    vec3 ambient;
    vec3 diffuse;
    vec3 specular;
};


#define MAX_POINT_LIGHTS 32
layout(set = 0, binding = 3) uniform PointLights {
    PointLight pointLights[MAX_POINT_LIGHTS];
};

layout(set = 0, binding = 4) uniform DirLightBlock {
    DirLight dirLight;
};

layout(set = 0, binding = 5) uniform SpotLightBlock {
    SpotLight spotLight;
};

layout(set = 0, binding = 6)  uniform sampler2D texture_diffuse1;
layout(set = 0, binding = 7)  uniform sampler2D texture_normal1;
layout(set = 0, binding = 10) uniform sampler2D texture_metallic_roughness1;
layout(set = 0, binding = 11) uniform sampler2D texture_ao1;
layout(set = 0, binding = 12) uniform sampler2D texture_emissive1;

layout(set = 0, binding = 1) uniform sampler2D shadowMap;
layout(set = 0, binding = 2) uniform Matrices {
    mat4 lightView;
    mat4 lightProjection;
};

vec3 CalcPointLight(PointLight light, vec3 normal, vec3 fragPos, vec3 viewDir, vec3 diffTex, vec3 specTex);
vec3 CalcSpotLight(SpotLight light, vec3 normal, vec3 fragPos, vec3 viewDir, vec3 diffTex, vec3 specTex);
vec3 CalcDirLight(DirLight light, vec3 normal, vec3 viewDir, vec3 diffTex, vec3 specTex, float shadow);

void main()
{
    // TBN
    vec3 T = normalize(Tangent);
    vec3 B = normalize(Bitangent);
    vec3 N = normalize(Normal);
    mat3 TBN = mat3(T, B, N);

    // Normal Map - fallback si no existe
    vec3 normalMap = texture(texture_normal1, TexCoord).rgb;
    vec3 norm = N;
    if (length(normalMap) > 0.1) {
        normalMap = normalMap * 2.0 - 1.0;
        norm = normalize(TBN * normalMap);
    }

    vec3 viewDir = normalize(params.viewPos - FragPos);

    // Base color - fallback a blanco
    vec4 diffTexture = texture(texture_diffuse1, TexCoord);
    vec3 diffColor = (length(diffTexture.rgb) > 0.01) ? diffTexture.rgb : vec3(1.0);
    
    // Metallic-Roughness - fallback a defaults
    vec3 metallicRoughness = texture(texture_metallic_roughness1, TexCoord).rgb;
    float metallic = (length(metallicRoughness) > 0.01) ? metallicRoughness.b : 0.0;
    
    // AO - fallback a 1.0 (sin oclusión)
    float ao = texture(texture_ao1, TexCoord).r;
    ao = (ao > 0.01) ? ao : 1.0;
    
    // Emissive - fallback a negro (no emite)
    vec3 emissive = texture(texture_emissive1, TexCoord).rgb;
    emissive = (length(emissive) > 0.01) ? emissive : vec3(0.0);
    
    vec3 specColor = mix(vec3(0.04), diffColor, metallic);

    // Light Position on Space
    vec4 posLightSpace = lightProjection * lightView * vec4(FragPos, 1.0);
    vec3 projCoords = posLightSpace.xyz / posLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5; // [-1,1] -> [0,1]

    // Calcular sombra con PCF (suavizado)
    float shadow = 0.0;
    if (projCoords.x >= 0.0 && projCoords.x <= 1.0 &&
        projCoords.y >= 0.0 && projCoords.y <= 1.0 &&
        projCoords.z >= 0.0 && projCoords.z <= 1.0) 
    {
        vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
        for(int x = -1; x <= 1; ++x)
        {
            for(int y = -1; y <= 1; ++y)
            {
                float pcfDepth = texture(shadowMap, projCoords.xy + vec2(x, y) * texelSize).r;
                float bias = 0.005;
                shadow += (pcfDepth < projCoords.z - bias) ? 1.0 : 0.0;  // CAMBIADO
            }
        }
        shadow /= 9.0;  // Promediar 9 muestras
    }

    vec3 result = CalcDirLight(dirLight, norm, viewDir, diffColor, specColor, shadow);

    for(int i = 0; i < params.nr_active_lights; i++)
        result += CalcPointLight(pointLights[i], norm, FragPos, viewDir, diffColor, specColor);

    result += CalcSpotLight(spotLight, norm, FragPos, viewDir, diffColor, specColor);
    result *= ao;
    result += emissive;

    FragColor = vec4(result, 1.0);
}

vec3 CalcDirLight(DirLight light, vec3 normal, vec3 viewDir, vec3 diffTex, vec3 specTex, float shadow)
{
    vec3 lightDir = normalize(-light.direction);
    
    // Ambient SIEMPRE visible (nunca en sombra)
    vec3 ambient  = max(light.ambient, vec3(0.15)) * diffTex;
    
    // Diffuse y Specular solo si NO hay sombra
    float diff = max(dot(normal, lightDir), 0.0);
    vec3 diffuse  = light.diffuse  * diff * diffTex * (1.0 - shadow);
    
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), params.shininess);
    vec3 specular = light.specular * spec * specTex * (1.0 - shadow);
    
    return (ambient + diffuse + specular);
}

vec3 CalcPointLight(PointLight light, vec3 normal, vec3 fragPos, vec3 viewDir, vec3 diffTex, vec3 specTex)
{
    vec3 lightDir = normalize(light.position - fragPos);

    // Diffuse
    float diff = max(dot(normal, lightDir), 0.0);

    // Specular
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), params.shininess);

    // Attenuation
    float distance = length(light.position - fragPos);
    float attenuation = 1.0 / (light.constant + light.linear * distance + (light.quadratic * (distance * distance)));    

    vec3 ambient  = light.ambient  * diffTex;
    vec3 diffuse  = light.diffuse  * diff * diffTex;
    vec3 specular = light.specular * spec * specTex;
    
    return (ambient + diffuse + specular) * attenuation;
}

vec3 CalcSpotLight(SpotLight light, vec3 normal, vec3 fragPos, vec3 viewDir, vec3 diffTex, vec3 specTex)
{
    vec3 lightDir = normalize(light.position - fragPos);
    float theta = dot(lightDir, normalize(-light.direction));
    float epsilon = light.cutOff - light.outerCutOff;
    float intensity = clamp((theta - light.outerCutOff) / epsilon, 0.0, 1.0);

    float diff = max(dot(normal, lightDir), 0.0);
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), params.shininess);

    vec3 ambient  = light.ambient  * diffTex;
    vec3 diffuse  = light.diffuse  * diff * diffTex;
    vec3 specular = light.specular * spec * specTex;
    
    return (ambient + (diffuse + specular) * intensity);
}