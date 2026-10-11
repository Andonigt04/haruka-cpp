/**
 * @file softbody_mesh.vert
 * @brief Una MALLA DE AUTOR dibujada con las posiciones de la tela XPBD (la piel de una criatura hecha
 *        en Blender): `SoftBodyRenderer::addMesh`. Posiciones relativas a la cámara, como `softbody.vert`.
 */
#version 450 core

layout(location = 0) in vec3 aPos;     // relativa a la cámara
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aColor;

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;
layout(location = 2) out vec2 UV;
layout(location = 3) out vec3 Color;

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 _cameraPos;     float _pad0;
    vec3 sunDirection;   float _pad1;
    vec3 sunLightColor;  float ambientStrength;
    int  enableHDR;
};

void main() {
    FragPos = aPos;
    Normal  = aNormal;
    UV      = aUV;
    Color   = aColor;
    gl_Position = projection * mat4(mat3(view)) * vec4(aPos, 1.0);
}
