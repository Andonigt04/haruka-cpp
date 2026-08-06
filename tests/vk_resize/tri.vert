#version 450
// Triángulo sin vertex buffer: posiciones/colores por gl_VertexIndex. Dynamic viewport/scissor
// en el pipeline → al redimensionar NO hay que recrear el pipeline, solo actualizar viewport.
layout(location = 0) out vec3 vColor;
vec2 positions[3] = vec2[](vec2(0.0, -0.6), vec2(0.6, 0.6), vec2(-0.6, 0.6));
vec3 colors[3]    = vec3[](vec3(1.0, 0.35, 0.2), vec3(0.25, 1.0, 0.35), vec3(0.3, 0.45, 1.0));
void main() {
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    vColor = colors[gl_VertexIndex];
}
