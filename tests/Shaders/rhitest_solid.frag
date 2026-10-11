/**
 * @file rhitest_solid.frag
 * @brief Pinta el color que venga en el UBO del binding 0.
 *
 * Es deliberadamente trivial: lo que audita el banco no es el shader, es si el UBO LLEGA. Si el
 * descriptor no está atado, el color sale distinto del esperado y el test lo dice.
 */
#version 450 core
layout(std140, binding = 0) uniform TestColor { vec4 uColor; };
layout(location = 0) out vec4 FragColor;
void main() { FragColor = uColor; }
