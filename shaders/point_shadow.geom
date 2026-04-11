#version 460 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 18) out;

uniform mat4 shadowMatrices[6];

in VS_OUT {
    vec3 WorldPos;
} gs_in[];

out GS_OUT {
    vec4 FragPos;
} gs_out;

void main()
{
    for(int face = 0; face < 6; ++face)
    {
        gl_Layer = face;
        
        for(int i = 0; i < 3; ++i)
        {
            gs_out.FragPos = vec4(gs_in[i].WorldPos, 1.0);
            gl_Position = shadowMatrices[face] * gs_out.FragPos;
            EmitVertex();
        }
        EndPrimitive();
    }
}