/**
 * @file deferred_light.frag
 * @brief Deferred lighting pass — resolves all lights from the GBuffer.
 *
 * Reads gPosition/gNormal/gAlbedoSpec/gEmissive and accumulates:
 *   - Ambient (constant 0.1 * albedo)
 *   - Directional light (position.length > 1000 = sun heuristic) with
 *     cascaded shadow maps (PCF 3×3, up to 4 cascades)
 *   - Point lights (up to 256) with quadratic attenuation
 *   - Omnidirectional point shadow (PCF disk, 8 taps)
 *   - Emissive contribution (* 0.5)
 *
 * In:  TexCoords (screen UV)
 * Out: FragColor (HDR lit scene)
 * Samplers: gPosition, gNormal, gAlbedoSpec, gEmissive, ssao,
 *           pointShadowMap (cubemap), cascadeShadowMaps[4]
 * UBO: Params, Matrices, Lights[256]
 */
#version 450 core

#extension GL_ARB_gpu_shader_fp64 : enable

layout(location = 0) out vec4 FragColor;
layout(location = 0) in vec2 TexCoords;

layout(set = 0, binding = 0) uniform sampler2D gPosition;
layout(set = 0, binding = 1) uniform sampler2D gNormal;
layout(set = 0, binding = 2) uniform sampler2D gAlbedoSpec;
layout(set = 0, binding = 3) uniform sampler2D gEmissive;
layout(set = 0, binding = 4) uniform sampler2D ssao; // reserved — AO not yet wired in main()
layout(set = 0, binding = 6) uniform sampler2DShadow cascadeShadowMaps[4];

layout(set = 0, binding = 7) uniform Params {
    vec3 viewPos;
    float farPlane;  // reserved — was used by point shadow (removed); kept to preserve UBO layout
    int numCascades;
    int numLights;
    vec3 lightPos;
    float _pad1; // UBO alignment padding (vec3 = 12 bytes, needs 4-byte fill before next member)
    float cascadeSplits[4];
} params;

layout(set = 0, binding = 8) uniform Matrices {
    mat4 view;
    mat4 cascadeLightSpaceMatrices[4];
};

struct Light {
    vec3 position;
    vec3 color;
};
layout(set = 0, binding = 9) uniform Lights {
    Light lights[256];
};

int getCascadeIndex(float viewDepth)
{
    for (int i = 0; i < params.numCascades; ++i) {
        if (viewDepth < params.cascadeSplits[i]) return i;
    }
    return max(params.numCascades - 1, 0);
}

float calculateCascadedVisibility(vec3 fragPosWorld, vec3 normal, vec3 lightDir, float viewDepth)
{
    if (params.numCascades <= 0) return 0.0;

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

    vec3 viewDir = normalize(params.viewPos - FragPos);
    float viewDepth = -(view * vec4(FragPos, 1.0)).z;

    // ===== DIRECT LIGHTING =====
    vec3 lighting = vec3(0.0);
    
    // Ambient
    vec3 ambient = 0.1 * Albedo * AO;
    lighting += ambient;

    for(int i = 0; i < params.numLights; ++i)
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