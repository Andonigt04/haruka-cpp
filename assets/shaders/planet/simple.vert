#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNorm;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec3 aColor;
layout(location = 4) in vec3 aClimate;
layout(std140, binding = 0) uniform SimplePlanetUBO {
    mat4 uMVP;
    vec4 uCenter;
    vec4 uLightDir;
    vec4 uLightColor;
    vec4 uAmbient;
    vec4 uExtra; // x = hasTex, y = tiling
    vec4 uDebug; // x = vista de depuración (ver TerrestrialPlanet::debugView)
};
out vec3 vNorm; out vec3 vFragPos; out vec3 vColor; out vec2 vUv; out vec3 vClimate;
void main() {
    vec3 worldPos = aPos + uCenter.xyz;
    vFragPos = worldPos; vNorm = aNorm; vColor = aColor; vUv = aUv; vClimate = aClimate;
    gl_Position = uMVP * vec4(worldPos, 1.0);
}
