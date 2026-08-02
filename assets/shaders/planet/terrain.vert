#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNorm;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec3 aColor;
layout(location = 4) in vec3 aClimate;
out vec3 cpPos; out vec3 cpColor; out vec3 cpClimate;
void main() { cpPos = aPos; cpColor = aColor; cpClimate = aClimate; }
