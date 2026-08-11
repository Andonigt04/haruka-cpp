#version 460 core
layout(location = 0) in vec3 vNorm; layout(location = 1) in vec3 vFragPos; layout(location = 2) in vec3 vColor; layout(location = 3) in vec2 vUv;
layout(std140, binding = 0) uniform SimplePlanetUBO {
    mat4 uMVP; vec4 uCenter; vec4 uLightDir; vec4 uLightColor; vec4 uAmbient; vec4 uExtra; vec4 uDebug;
    vec4 uTexAnchor;   // ancla planetaria de las UV de terreno (la usa biome.frag; ver planet.cpp)
};
layout(location = 0) out vec4 fragColor;
void main() {
    vec3 n = normalize(vNorm);
    float diff = max(dot(n, normalize(uLightDir.xyz)), 0.0);
    vec3 sky = vec3(0.09, 0.14, 0.22);
    vec3 L = normalize(uLightDir.xyz);
    vec3 V = -vFragPos;
    V = length(V) > 1e-6 ? normalize(V) : vec3(0.0, 0.0, 1.0);
    vec3 H = normalize(L + V);
    float sunD = clamp(diff * 1.6, 0.0, 1.0);
    float sheen = pow(max(dot(n, H), 0.0), 24.0) * 0.30;
    fragColor = vec4(vColor * (uAmbient.xyz + sky * (0.35 + 0.65 * sunD) + uLightColor.xyz * sunD * 1.6)
                     + uLightColor.xyz * sheen, 1.0);
}
