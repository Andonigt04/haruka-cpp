#version 460 core
layout(location = 0) in vec2 aLocal;      // offset en METROS dentro del plano tangente
out vec2 cLocal;
void main() { cLocal = aLocal; }
