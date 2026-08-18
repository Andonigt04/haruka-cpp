// Banco: proyecta con la matriz REAL del motor para poder observar el sentido de giro.
// Ver `testCullWindingWithProjection` en tests/rhi_test_main.cpp.
#version 450 core
layout(std140, binding = 0) uniform CullU { mat4 proj; } u;
layout(location = 0) in vec3 aPos;
void main() { gl_Position = u.proj * vec4(aPos, 1.0); }
