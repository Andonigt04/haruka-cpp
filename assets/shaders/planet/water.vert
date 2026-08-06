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
    vec4 uExtra;
    vec4 uDebug; // z = hay bake de altura (16)
};
// Cuerpos masivos que atraen/mueven el mar (siempre bindleado; uTide.x = 0 ⇒ neutro).
#include "lib/tidal.glsl"
out vec3 vNorm; out vec3 vFragPos; out vec3 vColor; out vec2 vUv; out vec3 vClimate;
void main() {
    // Flujo de superficie relativo al CENTRO del planeta (aPos está en la esfera del océano).
    vec3 centerRel = aPos;
    // Desplazamiento RADIAL por el campo de marea de los cuerpos masivos. Es GEOMETRÍA REAL, no
    // solo luz: el agua sube/baja alrededor de cada masa y, al moverse los cuerpos, las ondas
    // viajan. Sin cuerpos (uTide.x = 0) tidalHeight devuelve 0 → esfera idéntica a antes.
    float tide = tidalHeight(centerRel);
    vec3  disp = normalize(centerRel) * tide;
    vec3  worldPos = (centerRel + disp) + uCenter.xyz;
    vFragPos = worldPos; vNorm = aNorm; vColor = aColor; vUv = aUv; vClimate = aClimate;
    gl_Position = uMVP * vec4(worldPos, 1.0);
}