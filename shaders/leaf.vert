/**
 * @file leaf.vert
 * @brief Hojas instanciadas. Una malla de hoja (glTF) dibujada N veces con una
 *        matriz por instancia (posición/orientación/escala, rel. al parche) + tinte.
 *        Render camera-relativo: u_offset = origenParche - cámara.
 * In: aPos(0) aNormal(1) aUV(2) | iModel(3-6, instancia) iTint(7, instancia)
 */
#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in mat4 iModel; // ocupa 3,4,5,6
layout(location = 7) in vec3 iTint;

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;
layout(location = 2) out vec2 TexCoord;
layout(location = 3) out vec3 Tint;

layout(location = 10) uniform vec3 u_offset; // origenParche - cámara

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 _cameraPos;      float _pad0;
    vec3 sunDirection;    float _pad1;
    vec3 sunLightColor;   float ambientStrength;
    int  enableHDR;
};

void main() {
    vec3 wl     = (iModel * vec4(aPos, 1.0)).xyz; // posición rel. al parche
    vec3 camRel = u_offset + wl;
    FragPos  = camRel;
    Normal   = normalize(mat3(iModel) * aNormal);
    TexCoord = aUV;
    Tint     = iTint;
    gl_Position = projection * mat4(mat3(view)) * vec4(camRel, 1.0);
}
