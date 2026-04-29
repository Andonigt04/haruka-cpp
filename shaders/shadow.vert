#version 450 core

layout (location = 0) in vec3 aPos;

layout(set = 0, binding = 0) uniform Matrices {
    mat4 model;
    mat4 lightSpaceMatrix;
};

void main()
{
    gl_Position = lightSpaceMatrix * model * vec4(aPos, 1.0);
}