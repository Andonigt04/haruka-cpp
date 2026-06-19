/**
 * @file light_cube.frag
 * @brief Forward shading with optional procedural planet terrain albedo.
 *
 * When planetCenterAndFlag.w > 0.5 (useProceduralTerrain), replaces the flat
 * base color with a biome blend computed from latitude, normalized height, and
 * surface slope: sand → grass (height), grass → rock (slope), mix → snow (latitude+height).
 * Noise from hash31() breaks up biome edges.
 *
 * In:  Normal (loc 0), FragPos (loc 1)
 * Out: FragColor (loc 0)
 * UBOs: PerFrameData (binding 0), PerObjectData (binding 1)
 */
#version 450 core

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;

layout(location = 0) out vec4 FragColor;

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 cameraPos;      float _pad0;
    vec3 sunDirection;   float _pad1;
    vec3 sunLightColor;  float ambientStrength;
    int  enableHDR;
    int  enableBloom;
    int  enableSSAO;
    int  enableIBL;
    int  enableShadows;
    int  _pad3a; int _pad3b; int _pad3c; // 3 ints sueltos (no array std140 → moon alineada a 208)
};

layout(std140, binding = 1) uniform PerObjectData {
    mat4 model;
    vec4 baseColorAndPlanetRadius; // rgb = light color, a = planet radius
    vec4 planetCenterAndFlag;      // xyz = planet center, w = useProceduralTerrain (0/1)
};

float hash31(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

vec3 terrainAlbedo(vec3 worldPos, vec3 N) {
    vec3  planetCenter = planetCenterAndFlag.xyz;
    float planetRadius = baseColorAndPlanetRadius.a;

    vec3  local    = worldPos - planetCenter;
    float r        = length(local);
    float safeR    = max(r, 1e-3);
    vec3  dir      = local / safeR;
    float latitude = abs(dir.y);
    float refR     = max(planetRadius, 1.0);
    float h        = smoothstep(0.98, 1.02, r / refR);
    float slope    = 1.0 - max(dot(normalize(N), dir), 0.0);
    float n        = hash31(dir * 127.0) * 2.0 - 1.0;

    vec3 sand  = vec3(0.52, 0.42, 0.28);
    vec3 grass = vec3(0.18, 0.36, 0.16);
    vec3 rock  = vec3(0.42, 0.39, 0.36);
    vec3 snow  = vec3(0.90, 0.92, 0.95);

    vec3 col = mix(sand,  grass, smoothstep(0.10, 0.35, h + n * 0.05));
    col      = mix(col,   rock,  smoothstep(0.20, 0.55, slope + n * 0.06));
    float snowMask = smoothstep(0.62, 0.92, latitude + h * 0.25 + n * 0.03);
    col = mix(col, snow, snowMask);
    return col;
}

void main() {
    vec3 baseColor = baseColorAndPlanetRadius.rgb;

    // Emissive stars: full brightness, no shading
    if (planetCenterAndFlag.w > 1.5) {
        FragColor = vec4(baseColor, 1.0);
        return;
    }

    vec3 N = normalize(Normal);
    vec3 L = normalize(sunDirection);

    float diff = max(dot(N, L), 0.0);

    if (planetCenterAndFlag.w > 0.5) {
        baseColor = terrainAlbedo(FragPos, N);
    }

    float ambient = max(ambientStrength, 0.08);
    vec3  color   = ambient * baseColor
                  + diff * baseColor * max(sunLightColor, vec3(0.5));

    FragColor = vec4(color, 1.0);
}
