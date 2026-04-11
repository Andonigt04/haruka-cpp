#version 460 core

// Vertex attributes del mesh base
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 texCoord;

// Instance attributes (divisor=1, una vez por instancia)
layout(location = 3) in mat4 model;      // Columnas 3,4,5,6
layout(location = 7) in vec4 instanceColor;
layout(location = 8) in vec3 instanceScale;

// Uniforms
uniform mat4 view;
uniform mat4 projection;

// Output
out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoord;
out vec4 InstanceColor;

void main() {
    // Transformar posición a world space con instancing
    FragPos = vec3(model * vec4(position, 1.0));
    
    // Transformar normal
    Normal = normalize(mat3(transpose(inverse(model))) * normal);
    
    // UV directo
    TexCoord = texCoord;
    
    // Color de instancia
    InstanceColor = instanceColor;
    
    // Proyectar a screen space
    gl_Position = projection * view * vec4(FragPos, 1.0);
}
