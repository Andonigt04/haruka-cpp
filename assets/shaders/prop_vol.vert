/**
 * @file prop_vol.vert
 * @brief Vertex de MALLA INSTANCIADA QUE REPRESENTA UN VOLUMEN (bultos de un medio que dispersa: los
 *        cúmulos de `ShellScatter`). Lo mismo que `prop_inst.vert` (stream compacto de 40 B, cámara
 *        relativo, aire por vértice) más un SEGUNDO stream por instancia con la envolvente del volumen
 *        al que pertenece el bulto, para `lib/volume_light.glsl`. Sin viento ni partes rotas.
 *
 * In (per-vertex, binding 0):   aPos (0), aNormal (1), aColorPk (2), aUv (9)
 * In (per-instance, binding 1): lib/instance.glsl
 * In (per-instance, binding 2): iVolC (6) = centro de la envolvente (pase) · semieje horizontal
 *                               iVolR (8) = semieje vertical · libre
 * Out: Normal (0), FragPos (1), Color (2), TexCoord (3), aire (4, 5), envolvente (6, 7) → prop_vol.frag
 */
#version 450 core
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aNormal;
layout(location = 2) in vec4 aColorPk;
layout(location = 9) in vec2 aUv;

#include "lib/instance.glsl"
layout(location = 6) in vec4 iVolC;
layout(location = 8) in vec2 iVolR;

#include "lib/frame_ubo.glsl"
#include "lib/aerial.glsl"
#include "lib/prop_params.glsl"

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;
layout(location = 2) out vec3 Color;
layout(location = 3) out vec2 TexCoord;
layout(location = 4) out vec3 AerialT;
layout(location = 5) out vec3 AerialS;
layout(location = 6) flat out vec4 VolC;
layout(location = 7) flat out vec2 VolR;

void main() {
    const vec3 aColor = aColorPk.rgb * (aColorPk.a * 4.0);
    const vec4 q = harukaInstanceQuat();
    vec3 camRel = harukaInstancePoint(q, aPos) + u_originRel.xyz;
    FragPos  = camRel;
    Normal   = harukaInstanceNormal(q, aNormal.xyz);
    Color    = aColor * harukaInstanceTint();
    TexCoord = aUv;
    VolC     = vec4(iVolC.xyz + u_originRel.xyz, iVolC.w);
    VolR     = iVolR;
    harukaAerialTS(camRel, AerialT, AerialS);
    gl_Position = projection * view * vec4(camRel, 1.0);
}
