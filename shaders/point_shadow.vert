#version 450 core

layout(location = 0) in vec3 aPos;

layout(location = 0) out vec3 WorldPos;

layout(set = 0, binding = 0) uniform Matrices {
    mat4 model;
};

void main()
{
    vec3 worldPos = vec3(model * vec4(aPos, 1.0));
    WorldPos = worldPos;
    gl_Position = vec4(worldPos, 1.0);
}