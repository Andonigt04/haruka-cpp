#version 460 core
#extension GL_ARB_gpu_shader_fp64 : enable

out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D gPosition;
uniform sampler2D gNormal;
uniform sampler2D gAlbedoSpec;
uniform sampler2D gEmissive;
uniform sampler2D ssao;
uniform samplerCube pointShadowMap;
uniform sampler2DShadow cascadeShadowMaps[4];

uniform vec3 viewPos;
uniform mat4 view;
uniform vec3 lightPos;
uniform float farPlane;
uniform mat4 cascadeLightSpaceMatrices[4];
uniform float cascadeSplits[4];
uniform int numCascades;

struct Light {
    vec3 position;
    vec3 color;
};
uniform Light lights[256];  // Aumentado de 32 a 256
uniform int numLights;

const vec3 sampleOffsetDirections[8] = vec3[]
(
   vec3( 1,  1,  1), vec3( 1, -1,  1), vec3(-1, -1,  1), vec3(-1,  1,  1), 
   vec3( 1,  1, -1), vec3( 1, -1, -1), vec3(-1, -1, -1), vec3(-1,  1, -1)
);

float ShadowCalculation(vec3 FragPos)
{
    vec3 fragToLight = FragPos - lightPos;
    float currentDepth = length(fragToLight);

    float shadow = 0.0;
    const float bias = 0.05;
    const int samples = 8;
    const float diskRadius = 0.01;

    for(int i = 0; i < samples; ++i)
    {
        float closestDepth = texture(pointShadowMap, normalize(fragToLight + sampleOffsetDirections[i] * diskRadius)).r;
        closestDepth *= farPlane;
        if(currentDepth - bias > closestDepth)
            shadow += 1.0;
    }
    shadow /= float(samples);
    return shadow;
}

int getCascadeIndex(float viewDepth)
{
    for (int i = 0; i < numCascades; ++i) {
        if (viewDepth < cascadeSplits[i]) return i;
    }
    return max(numCascades - 1, 0);
}

float calculateCascadedVisibility(vec3 fragPosWorld, vec3 normal, vec3 lightDir, float viewDepth)
{
    if (numCascades <= 0) return 0.0;

    int cascadeIndex = getCascadeIndex(viewDepth);
    vec4 fragPosLightSpace = cascadeLightSpaceMatrices[cascadeIndex] * vec4(fragPosWorld, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / max(fragPosLightSpace.w, 0.00001);
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.z > 1.0 || projCoords.x < 0.0 || projCoords.x > 1.0 || projCoords.y < 0.0 || projCoords.y > 1.0)
        return 0.0;

    float ndotl = max(dot(normal, lightDir), 0.0);
    float bias = max(0.0015 * (1.0 - ndotl), 0.0004);

    float visibility = 0.0;
    vec2 texelSize = 1.0 / vec2(textureSize(cascadeShadowMaps[cascadeIndex], 0));
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            vec2 offset = vec2(x, y) * texelSize;
            visibility += texture(cascadeShadowMaps[cascadeIndex], vec3(projCoords.xy + offset, projCoords.z - bias));
        }
    }
    return visibility / 9.0;
}

void main()
{
    vec3 FragPos = texture(gPosition, TexCoords).rgb;
    vec3 Normal  = normalize(texture(gNormal, TexCoords).rgb);
    vec3 Albedo  = texture(gAlbedoSpec, TexCoords).rgb;
    float Spec   = texture(gAlbedoSpec, TexCoords).a;
    vec3 Emissive = texture(gEmissive, TexCoords).rgb;
    float AO = 1.0;

    vec3 viewDir = normalize(viewPos - FragPos);
    float viewDepth = -(view * vec4(FragPos, 1.0)).z;

    // ===== DIRECT LIGHTING =====
    vec3 lighting = vec3(0.0);
    
    // Ambient
    vec3 ambient = 0.1 * Albedo * AO;
    lighting += ambient;

    for(int i = 0; i < numLights; ++i)
    {
        // Detectar si es DirectionalLight (posición muy lejana, > 1000)
        float dist = length(lights[i].position);
        
        if (dist > 1000.0) {
            // === DIRECTIONAL LIGHT (Sun) ===
            vec3 lightDir = normalize(lights[i].position);
            float diff = max(dot(Normal, lightDir), 0.0);
            float visibility = calculateCascadedVisibility(FragPos, Normal, lightDir, viewDepth);

            vec3 halfwayDir = normalize(lightDir + viewDir);
            float spec = pow(max(dot(Normal, halfwayDir), 0.0), 32.0) * Spec;

            vec3 radiance = lights[i].color;

            lighting += visibility * (diff * Albedo + spec * vec3(0.5)) * radiance;
        } else {
            // === POINT LIGHT ===
            vec3 lightDir = normalize(lights[i].position - FragPos);
            float diff = max(dot(Normal, lightDir), 0.0);

            vec3 halfwayDir = normalize(lightDir + viewDir);
            float spec = pow(max(dot(Normal, halfwayDir), 0.0), 32.0) * Spec;

            float distance = length(lights[i].position - FragPos);
            float attenuation = 1.0 / (distance * distance + 0.001);

            vec3 radiance = lights[i].color * attenuation;

            lighting += (diff * Albedo + spec * vec3(0.5)) * radiance;
        }
    }
    
    lighting += Emissive * 0.5;
    FragColor = vec4(lighting, 1.0);
}