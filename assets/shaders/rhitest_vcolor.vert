/**
 * @file rhitest_vcolor.vert
 * @brief Triángulo con COLOR POR VÉRTICE desde un vertex buffer (pos vec3 + color vec3).
 *
 * Audita que un atributo de vértice que NO es la posición llegue al fragment. Los props del mundo
 * sacan su marrón/verde del color por vértice, y en Vulkan salían blancos con las texturas
 * correctas — o sea que el sospechoso es justo este camino.
 */
#version 450 core
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec3 aColor;   // location 2, igual que en prop_inst.vert
layout(location = 0) out vec3 vColor;
void main() { vColor = aColor; gl_Position = vec4(aPos, 1.0); }
