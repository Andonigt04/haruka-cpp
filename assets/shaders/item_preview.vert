/**
 * @file item_preview.vert
 * @brief Icono 3D de un item para las casillas del inventario.
 *
 * Existe como FICHERO y no como GLSL en línea porque el preview vivía en OpenGL crudo (glCreateShader
 * + FBO propio) y bajo Vulkan no hay contexto GL: las casillas salían sin icono. El GLSL en línea del
 * RHI es solo-GL (ver PipelineDesc, opción C), así que portarlo exige shaders en disco que el build
 * compile a .spv.
 *
 * El UBO sustituye a los `glUniformMatrix4fv` sueltos: en Vulkan no existen uniforms fuera de bloque.
 */
#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNrm;
layout(location = 2) in vec3 aCol;

layout(std140, binding = 0) uniform ItemPreviewUBO {
    mat4 uMVP;     // proyección × vista × modelo (con la Y ya invertida si el backend es Vulkan)
    mat4 uModel;   // solo para llevar la normal al espacio del icono
};

layout(location = 0) out vec3 vN;
layout(location = 1) out vec3 vC;

void main() {
    vN = mat3(uModel) * aNrm;
    vC = aCol;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
