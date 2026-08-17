#version 460 core
// Gemelo de `clipmap.vert`: el agua reusa LA MISMA rejilla de parches que el terreno (mismo VB, mismo
// IB, mismos ClipParams por anillo). Solo cambia qué superficie se evalúa después.
layout(location = 0) in vec2 aLocal;      // offset en METROS dentro del plano tangente
layout(location = 0) out vec2 cLocal;
void main() { cLocal = aLocal; }
