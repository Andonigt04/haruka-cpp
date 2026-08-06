#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;

layout(location = 0) out vec2 vUV;

layout(std140, binding = 0) uniform PanelTransform {
    mat4 model;
    mat4 view;
    mat4 proj;
    float focused;
    float _pad0, _pad1, _pad2;
};

void main() {
    vUV = aUV;
    gl_Position = proj * view * model * vec4(aPos, 1.0);
}
