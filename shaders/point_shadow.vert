#version 460 core
layout(location = 0) in vec3 aPos;

out VS_OUT {
    vec3 WorldPos;
} vs_out;

uniform mat4 model;

void main()
{
    vec3 worldPos = vec3(model * vec4(aPos, 1.0));
    vs_out.WorldPos = worldPos;
    gl_Position = vec4(worldPos, 1.0);
}