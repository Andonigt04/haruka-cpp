#version 450 core

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;

layout(location = 0) out vec4 FragColor;

layout(location = 3) uniform vec3  lightColor;
layout(location = 4) uniform vec3  sunDirection;
layout(location = 5) uniform vec3  sunLightColor;
layout(location = 6) uniform float ambientStrength;
layout(location = 7) uniform bool  useProceduralTerrain;
layout(location = 8) uniform vec3  planetCenter;
layout(location = 9) uniform float planetRadius;

float hash31(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

vec3 terrainAlbedo(vec3 worldPos, vec3 N) {
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
    vec3 N = normalize(Normal);
    vec3 L = normalize(sunDirection);

    float diff = max(dot(N, L), 0.0);

    vec3 baseColor = lightColor;
    if (useProceduralTerrain) {
        baseColor = terrainAlbedo(FragPos, N);
    }

    float ambient = max(ambientStrength, 0.08);
    vec3  color   = ambient * baseColor
                  + diff * baseColor * max(sunLightColor, vec3(0.5));

    FragColor = vec4(color, 1.0);
}
