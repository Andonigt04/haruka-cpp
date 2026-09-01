#version 460 core
#extension GL_GOOGLE_include_directive : require
/**
 * @file terrain_node_water.frag
 * @brief El agua del pase de nodos. Comparte el SOMBREADO del mar, no su geometria.
 *
 * ⚠️ NO REUSA `ocean.frag` Y ES DELIBERADO. Aquel lee `ClipParams` (binding 13) y un `vLoc` en
 * coordenadas de ANILLO para descartar el hueco — o sea, depende de la rejilla del clipmap, que es
 * justo lo que esta migracion viene a quitar. Reusarlo habria dejado el resto vivo por la puerta de
 * atras. Lo que si se comparte es `harukaOceanShade`, que es el aspecto y no el sitio.
 */
#include "lib/ocean_params.glsl"
#include "lib/ocean_shade.glsl"      // harukaOceanShade: absorcion, Fresnel, espuma

layout(std140, binding = 0) uniform NodeDraw {
    mat4  uMVP; vec4 uCenter; vec4 uCenterLo; vec4 uLod; ivec4 uGrid; ivec4 uEdgeUnused;
    vec4  uMisc; vec4 uShade; vec4 uTexAnchor; vec4 uLightDir;
};

layout(location = 0) in vec3  vNormal;
layout(location = 1) in vec3  vFragPos;
layout(location = 2) in float vDepth;
layout(location = 3) in float vFoam;

layout(location = 0) out vec4 fragColor;

void main() {
    // Aqui no hay agua: el fondo asoma. Mismo criterio que `ocean.frag` — la orilla la decide la
    // PROFUNDIDAD, no un recorte de geometria, asi que sube y baja con la marea sola.
    if (vDepth <= 0.0) discard;
    const vec3 wp = vFragPos - uCenter.xyz - uCenterLo.xyz;
    fragColor = harukaOceanShade(wp, vFragPos, normalize(vNormal), normalize(uLightDir.xyz),
                                 vec3(1.0), vec3(0.10, 0.13, 0.18), vDepth, vFoam);
}
