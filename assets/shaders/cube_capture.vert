/**
 * @file cube_capture.vert
 * @brief Vertex de cualquier pase que pinte UNA CARA DE UN CUBEMAP desde dentro: el cubo unidad se
 *        dibuja con la vista de esa cara y el fragment recibe la DIRECCION (`WorldPos`).
 *
 * Lo comparten los tres horneados de IBL (`equirect_to_cubemap`, `irradiance_convolution`,
 * `prefilter_env`), que eran tres copias identicas; sirve igual para cualquier otro bake a cubemap
 * (cielo, reflejos, mapas de sombra omni) que solo necesite la direccion por pixel.
 *
 * In:  aPos (location 0) — vertice del cubo unidad
 * Out: WorldPos (location 0) — la direccion de muestreo, sin normalizar
 * UBO: Matrices { projection, view } (binding 0) — proyeccion de 90° y vista de la cara
 */
#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 0) out vec3 WorldPos;

layout(std140, binding = 0) uniform Matrices {
    mat4 projection;
    mat4 view;
};

void main()
{
    WorldPos = aPos;
    gl_Position = projection * view * vec4(WorldPos, 1.0);
}
