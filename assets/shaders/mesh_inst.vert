/**
 * @file mesh_inst.vert
 * @brief Vertex INSTANCIADO para mallas con el `Vertex` de ESCENA (posicion, normal, UV; sin color
 *        por vertice): piezas de construccion, objetos de NPC, primitivas… cualquier modelo que se
 *        repita con el MISMO material. Un draw por malla del modelo.
 *
 * Es el adaptador del formato de malla, nada mas: sale con los mismos varyings que `prop_inst.vert`
 * y se empareja con el MISMO fragment (`prop_inst.frag`, el toon de malla instanciada). La diferencia
 * entre un prop y una pieza de obra es de MATERIAL (rim, especular), y eso viaja en `PropParams`.
 *
 * In (per-vertex, binding 0):  aPos (0), aNormal (1), aUv (2)  [subconjunto del Vertex de escena]
 * In (per-instance, binding 1): ver lib/instance.glsl (pos 3, giro 4, escala 5, tinte 7)
 * Out: Normal (0), FragPos (1), Color (2), TexCoord (3) → prop_inst.frag
 * UBOs: PerFrameData (binding 0) + PropParams (binding 6)
 */
#version 450 core
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;

#include "lib/instance.glsl"      // posición + giro + escala + tinte por instancia (40 B)
#include "lib/frame_ubo.glsl"     // PerFrameData (binding 0)
#include "lib/aerial.glsl"        // el aire, por vértice
#include "lib/prop_params.glsl"   // PropParams (binding 6): material, origen, aire

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;
layout(location = 2) out vec3 Color;
layout(location = 3) out vec2 TexCoord;
// EL AIRE entre el ojo y el vértice (lib/aerial.glsl, por vértice: ver `harukaAerialTS`).
layout(location = 4) out vec3 AerialT;
layout(location = 5) out vec3 AerialS;

void main() {
    const vec4 q = harukaInstanceQuat();
    const vec3 camRel = harukaInstancePoint(q, aPos) + u_originRel.xyz;
    FragPos  = camRel;
    Normal   = harukaInstanceNormal(q, aNormal);   // R·(n/s): sin inverse() por vértice
    Color    = harukaInstanceTint();
    TexCoord = aUv;
    harukaAerialTS(camRel, AerialT, AerialS);
    gl_Position = projection * view * vec4(camRel, 1.0);
}
