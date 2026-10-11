/**
 * @file rhitest_tex.frag
 * @brief Muestrea la textura del binding 0 y la saca tal cual.
 *
 * Audita que el CONTENIDO de una textura sobreviva a la subida. El banco ya probaba que se CREAN
 * por formato, que es otra cosa: una textura puede crearse y llegar en blanco al shader (es
 * exactamente lo que se sospecha en Vulkan, con los props saliendo blancos).
 */
#version 450 core
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 FragColor;
layout(std140, binding = 0) uniform TestUv { vec4 uUv; };   // xy = coordenada a muestrear
void main() { FragColor = texture(u_tex, uUv.xy); }
