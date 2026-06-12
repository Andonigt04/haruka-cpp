/**
 * @file prop.vert
 * @brief Vertex shader para props del mundo (árboles, rocas). Cada prop es una
 *        malla en espacio local (ya orientada al "up" de la superficie y escalada);
 *        u_propOffset = centroDelProp - cameraPos la coloca camera-relativa.
 * In: aPos (0), aNormal (1), aColor (2)
 */
#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;
layout(location = 2) out vec3 Color;
layout(location = 3) out vec3 LocalPos; // posición rel. al parche (estable) → triplanar sin swim

layout(location = 10) uniform vec3 u_propOffset; // centro - cámara

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 _cameraPos;      float _pad0;
    vec3 sunDirection;    float _pad1;
    vec3 sunLightColor;   float ambientStrength;
    int  enableHDR;
};

void main() {
    vec3 camRel = u_propOffset + aPos;
    FragPos  = camRel;
    LocalPos = aPos;
    Normal   = normalize(aNormal);
    Color    = aColor;
    gl_Position = projection * mat4(mat3(view)) * vec4(camRel, 1.0);
}
