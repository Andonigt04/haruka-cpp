#version 450 core

layout(location = 0) in vec4 FragPos;

layout(set = 0, binding = 0) uniform Params {
    vec3 lightPos;
    float farPlane;
};

void main()
{
    float lightDistance = length(FragPos.xyz - lightPos);
    lightDistance = lightDistance / farPlane;
    gl_FragDepth = lightDistance;
}