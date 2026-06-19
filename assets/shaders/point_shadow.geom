#version 450 core

layout(triangles) in;
layout(triangle_strip, max_vertices = 18) out;

layout(location = 4) uniform mat4 shadowMatrices[6];

layout(location = 0) in vec3 WorldPos[];
layout(location = 0) out vec4 FragPos;

void main()
{
    for(int face = 0; face < 6; ++face)
    {
        gl_Layer = face;
        
        for(int i = 0; i < 3; ++i)
        {
            FragPos = vec4(WorldPos[i], 1.0);
            gl_Position = shadowMatrices[face] * FragPos;
            EmitVertex();
        }
        EndPrimitive();
    }
}