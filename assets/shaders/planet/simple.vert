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
    vec4 uTexAnchor;   // ancla planetaria de las UV de terreno (la usa biome.frag; ver planet.cpp)
};
layout(location = 0) out vec3 vNorm; layout(location = 1) out vec3 vFragPos; layout(location = 2) out vec3 vColor; layout(location = 3) out vec2 vUv; layout(location = 4) out vec3 vClimate;
// Este vertex se empareja con `biome.frag` en el pipeline de bioma SIN teselar (`s_biomePipeline`,
// ver planet.cpp). `biome.frag` declara `vSurfKind` en la location 5 y aquí no existía: el programa
// NO ENLAZABA ("vSurfKind not declared as input from previous stage") y esa ruta se quedaba sin
// pipeline. Vale 1.0 porque dibuja la MALLA BASE — lo mismo que pone `terrain.tese`, que es esa misma
// superficie ya teselada. El clipmap y el anillo cercano ponen 0.0.
layout(location = 5) out float vSurfKind;
void main() {
    vec3 worldPos = aPos + uCenter.xyz;
    vFragPos = worldPos; vNorm = aNorm; vColor = aColor; vUv = aUv; vClimate = aClimate;
    vSurfKind = 1.0;
    gl_Position = uMVP * vec4(worldPos, 1.0);
}
