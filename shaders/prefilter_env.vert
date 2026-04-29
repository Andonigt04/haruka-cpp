#version 450 core

layout (location = 0) in vec3 aPos;
layout (location = 0) out vec3 WorldPos;

layout(set = 0, binding = 0) uniform Matrices {
    mat4 projection;
    mat4 view;
};

void main()
{
    WorldPos = aPos;
    gl_Position = projection * view * vec4(WorldPos, 1.0);
}