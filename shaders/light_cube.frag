#version 450 core

layout(location = 0) out vec4 FragColor;
layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;

layout(set = 0, binding = 0) uniform sampler2D shadowMap;
layout(set = 0, binding = 1) uniform sampler2DShadow cascadeShadowMaps[4];

layout(set = 0, binding = 2) uniform Params {
    vec3 lightColor;
    vec3 sunDirection;
    vec3 sunLightColor;
    float ambientStrength;
    int numCascades;
    bool useShadowMap;
    bool useProceduralTerrain;
    float cascadeSplits[4];
} params;

layout(set = 0, binding = 3) uniform Matrices {
    mat4 lightSpaceMatrix;
    mat4 view;
    mat4 cascadeLightSpaceMatrices[4];
};

float hash31(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

vec3 terrainAlbedo(vec3 worldPos, vec3 N)
{
    vec3 dir = normalize(worldPos);
    float latitude = abs(dir.y);

    // Approximate altitude banding from distance to origin, stabilized with tiny normalization.
    float r = length(worldPos);
    float h = smoothstep(0.98, 1.02, r / max(r, 1e-5));

    float slope = 1.0 - max(dot(normalize(N), dir), 0.0);
    float n = hash31(dir * 127.0) * 2.0 - 1.0;

    vec3 sand  = vec3(0.52, 0.42, 0.28);
    vec3 grass = vec3(0.18, 0.36, 0.16);
    vec3 rock  = vec3(0.42, 0.39, 0.36);
    vec3 snow  = vec3(0.90, 0.92, 0.95);

    // Base blend: sand -> grass -> rock
    vec3 col = mix(sand, grass, smoothstep(0.10, 0.35, h + n * 0.05));
    col = mix(col, rock, smoothstep(0.20, 0.55, slope + n * 0.06));

    // Snow at high latitude and high altitude
    float snowMask = smoothstep(0.62, 0.92, latitude + h * 0.25 + n * 0.03);
    col = mix(col, snow, snowMask);

    return col;
}

float calculateShadow(vec3 normal, vec3 lightDir)
{
    if (!params.useShadowMap) return 0.0;

    vec4 fragPosLightSpace = lightSpaceMatrix * vec4(FragPos, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / max(fragPosLightSpace.w, 0.00001);
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.z > 1.0 || projCoords.x < 0.0 || projCoords.x > 1.0 || projCoords.y < 0.0 || projCoords.y > 1.0)
        return 0.0;

    float currentDepth = projCoords.z;
    float ndotl = max(dot(normal, lightDir), 0.0);
    float bias = max(0.0035 * (1.0 - ndotl), 0.0008);

    float shadow = 0.0;
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float pcfDepth = texture(shadowMap, projCoords.xy + vec2(x, y) * texelSize).r;
            shadow += (currentDepth - bias > pcfDepth) ? 1.0 : 0.0;
        }
    }
    return shadow / 9.0;
}

int getCascadeIndex(float viewDepth)
{
    for (int i = 0; i < params.numCascades; ++i) {
        if (viewDepth < params.cascadeSplits[i]) return i;
    }
    return max(params.numCascades - 1, 0);
}

float calculateCascadedShadow(vec3 normal, vec3 lightDir)
{
    if (params.numCascades <= 0) return 0.0;

    float viewDepth = -(view * vec4(FragPos, 1.0)).z;
    int cascadeIndex = getCascadeIndex(viewDepth);

    vec4 fragPosLightSpace = cascadeLightSpaceMatrices[cascadeIndex] * vec4(FragPos, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / max(fragPosLightSpace.w, 0.00001);
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.z > 1.0 || projCoords.x < 0.0 || projCoords.x > 1.0 || projCoords.y < 0.0 || projCoords.y > 1.0)
        return 0.0;

    float ndotl = max(dot(normal, lightDir), 0.0);
    float bias = max(0.0015 * (1.0 - ndotl), 0.0004);

    float shadow = 0.0;
    vec2 texelSize = 1.0 / vec2(textureSize(cascadeShadowMaps[cascadeIndex], 0));
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            vec2 offset = vec2(x, y) * texelSize;
            shadow += texture(cascadeShadowMaps[cascadeIndex], vec3(projCoords.xy + offset, projCoords.z - bias));
        }
    }
    return shadow / 9.0;
}

void main() {
    vec3 N = normalize(Normal);
    vec3 L = normalize(params.sunDirection);

    float diff = max(dot(N, L), 0.0);
    float shadow = 0.0;
    if (diff > 0.0) {
        shadow = (params.numCascades > 0) ? calculateCascadedShadow(N, L) : calculateShadow(N, L);
    }

    vec3 baseColor = params.lightColor;
    if (params.useProceduralTerrain) {
        baseColor = terrainAlbedo(FragPos, N);
    }

    vec3 ambient = max(params.ambientStrength, 0.08) * baseColor;
    vec3 direct = (1.0 - shadow) * diff * baseColor * max(params.sunLightColor, vec3(0.5));

    FragColor = vec4(ambient + direct, 1.0);
}