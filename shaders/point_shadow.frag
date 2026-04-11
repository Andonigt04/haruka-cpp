#version 460 core

in GS_OUT {
    vec4 FragPos;
} fs_in;

uniform vec3 lightPos;
uniform float farPlane;

void main()
{
    float lightDistance = length(fs_in.FragPos.xyz - lightPos);
    
    // Normalizar a [0, 1]
    lightDistance = lightDistance / farPlane;
    
    gl_FragDepth = lightDistance;
}