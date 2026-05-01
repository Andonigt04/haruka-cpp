/**
 * @file instancing.vert
 * @brief GPU-instanced vertex shader.
 *
 * Each instance supplies its own model matrix (locations 3-6, mat4 occupies
 * 4 attribute slots), per-instance color (loc 7), and scale (loc 8).
 * Normal is corrected for non-uniform scaling via inverse-transpose.
 *
 * In (per-vertex):   position, normal, texCoord
 * In (per-instance): model (mat4), instanceColor, instanceScale
 * Out: FragPos, Normal, TexCoord, InstanceColor → instancing.frag
 * UBO: Matrices { view, projection }
 */
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
layout(set = 0, binding = 0) uniform Matrices {
    mat4 view;
    mat4 projection;
};

// Output
layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec3 Normal;
layout(location = 2) out vec2 TexCoord;
layout(location = 3) out vec4 InstanceColor;

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
