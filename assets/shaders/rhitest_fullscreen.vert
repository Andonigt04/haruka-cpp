/**
 * @file rhitest_fullscreen.vert
 * @brief Triángulo de pantalla completa para el banco del RHI. Sin VBO: sale de gl_VertexID
 *        (el backend Vulkan lo reescribe a gl_VertexIndex al compilar a SPIR-V).
 */
#version 450 core
void main()
{
    vec2 ndc = vec2((gl_VertexID == 1) ? 3.0 : -1.0,
                    (gl_VertexID == 2) ? 3.0 : -1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
