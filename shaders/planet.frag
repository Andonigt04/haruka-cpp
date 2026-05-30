/**
 * @file planet.frag
 * @brief Fragment shader for planetary terrain chunks.
 *
 * Checkerboard pattern from per-face UVs. Both Procedural and Manual modes
 * use the same visual for now (different tint to distinguish them).
 * u_terrainMode: 0 = Procedural, 1 = Manual
 */
#version 450 core

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;
layout(location = 2) in vec2 TexCoord;

layout(location = 11) uniform int u_terrainMode;

// Only the fields actually used from the per-frame UBO.
// Must preserve std140 offsets: view(0), projection(64),
// cameraPos+pad(128), sunDirection+pad(144), sunLightColor+ambientStrength(160), enableHDR(176).
layout(std140, binding = 0) uniform PerFrameData {
    mat4  view;
    mat4  projection;
    vec3  _cameraPos;   float _pad0;
    vec3  sunDirection; float _pad1;
    vec3  sunLightColor; float ambientStrength;
    int   enableHDR;
};

// 5 tiles per metre → tile edge = 0.2 m.
// FragPos is camera-relative world position in metres, so dividing by 0.2
// gives a grid that matches real-world scale regardless of LOD or UV layout.
const float TILE_SIZE = 0.2;

const vec3 PROC_DARK  = vec3(0.18, 0.22, 0.18);
const vec3 PROC_LIGHT = vec3(0.72, 0.78, 0.70);
const vec3 MAN_DARK   = vec3(0.18, 0.18, 0.28);
const vec3 MAN_LIGHT  = vec3(0.65, 0.68, 0.82);

void main() {
    // Use two axes of the camera-relative world position for the checker grid.
    // At the north pole the terrain lies in the XZ plane, so X and Z give
    // the two independent surface directions.  On other faces Y replaces one.
    vec3 absPos = abs(Normal);   // Normal is the sphere-outward unit direction
    float checker;
    if (absPos.y >= absPos.x && absPos.y >= absPos.z) {
        // Top / bottom face: use XZ
        checker = mod(floor(FragPos.x / TILE_SIZE) + floor(FragPos.z / TILE_SIZE), 2.0);
    } else if (absPos.x >= absPos.y && absPos.x >= absPos.z) {
        // Front / back face: use YZ
        checker = mod(floor(FragPos.y / TILE_SIZE) + floor(FragPos.z / TILE_SIZE), 2.0);
    } else {
        // Left / right face: use XY
        checker = mod(floor(FragPos.x / TILE_SIZE) + floor(FragPos.y / TILE_SIZE), 2.0);
    }

    vec3 dark  = (u_terrainMode == 1) ? MAN_DARK  : PROC_DARK;
    vec3 light = (u_terrainMode == 1) ? MAN_LIGHT : PROC_LIGHT;
    vec3 baseColor = mix(dark, light, checker);

    vec3 N = normalize(Normal);
    vec3 L = normalize(sunDirection);
    vec3 V = normalize(-FragPos);
    vec3 H = normalize(L + V);

    float ndl  = max(dot(N, L), 0.0);
    float spec = pow(max(dot(N, H), 0.0), 32.0);

    vec3 color = ambientStrength * baseColor
               + 0.7 * ndl * sunLightColor * baseColor
               + sunLightColor * 0.12 * spec;

    if (enableHDR != 0) {
        color = color / (color + vec3(1.0));
        color = pow(color, vec3(1.0 / 2.2));
    } else {
        color = clamp(color, 0.0, 1.0);
    }

    FragColor = vec4(color, 1.0);
}
