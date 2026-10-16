/**
 * @file simple.vert
 * @brief Forward-rendering vertex shader.
 *
 * Transforms position to world space (FragPos) and clip space.
 * Normal is corrected for non-uniform scaling via the inverse-transpose
 * of the model matrix.
 * Used with final.frag / light_cube.frag.
 *
 * In:  aPos (loc 0), aNormal (loc 1), aTexCoords (loc 2)
 * Out: Normal (loc 0), FragPos (loc 1), TexCoord (loc 2)
 * UBOs: PerFrameData (binding 0), PerObjectData (binding 1)
 */
#version 450 core
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;
layout(location = 2) out vec2 TexCoord;

#include "lib/frame_ubo.glsl"   // PerFrameData (binding 0)

// El bloque debe declararse IDÉNTICO en TODAS las etapas del mismo programa (este .vert se linka
// con final.frag y con preview.frag): si una declara menos campos, GL da "definitions of uniform
// block do not match" AL LINKAR, no al compilar, y el pase se queda mudo.
layout(std140, binding = 1) uniform PerObjectData {
    mat4 model;
    vec4 baseColorAndPlanetRadius; // rgb = base color, a = planet radius
    vec4 planetCenterAndFlag;      // xyz = planet center, w = useProceduralTerrain (0/1)
    vec4 materialPBR;              // x=metallic y=roughness z=ao w=máscara de texturas
    vec4 materialEmission;         // rgb = emisión
};

void main() {
    vec4 worldPos = model * vec4(aPos, 1.0);
    FragPos = worldPos.xyz;
    TexCoord = aTexCoords;

    mat3 normalMatrix = mat3(transpose(inverse(model)));
    Normal = normalize(normalMatrix * aNormal);

    gl_Position = projection * view * worldPos;
}
