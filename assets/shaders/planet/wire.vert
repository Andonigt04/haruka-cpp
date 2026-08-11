#version 460 core
layout(location = 0) in vec3 aPos;
layout(std140, binding = 0) uniform SimplePlanetUBO {
    mat4 uMVP;
    vec4 uCenter;
    vec4 uLightDir;
    vec4 uLightColor;
    vec4 uAmbient;
    vec4 uExtra;
    vec4 uDebug;
    vec4 uTexAnchor;   // ancla planetaria de las UV de terreno (la usa biome.frag; ver planet.cpp)
};
void main() { gl_Position = uMVP * vec4(aPos + uCenter.xyz, 1.0); }
