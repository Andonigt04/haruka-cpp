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
    vec4  uMisc; vec4 uShade; vec4 uTexAnchor; vec4 uLightDir; vec4 uAerial; vec4 uStrideRamp;
};

layout(location = 0) in vec3  vNormal;
layout(location = 1) in vec3  vFragPos;
layout(location = 2) in float vDepth;
layout(location = 3) in float vFoam;
layout(location = 4) flat in int vWStride;
layout(location = 5) flat in int vWLevel;
layout(location = 6) in float vSheet;

layout(location = 0) out vec4 fragColor;

void main() {
    // Aqui no hay agua: el fondo asoma. Mismo criterio que `ocean.frag` — la orilla la decide la
    // PROFUNDIDAD, no un recorte de geometria, asi que sube y baja con la marea sola.
    // A lake's sheet is not discarded on its shore: the terrain's depth cuts it per pixel (see the
    // vert). Where the interpolated depth says "dry" but the sheet is visible, the ground there is
    // below the lake: shallow water.
    // Only a triangle that is ALL sheet skips the depth rule. One with a dry vertex (the band's end)
    // keeps it: that vertex lies on its own ground, maybe metres below the lake, and with `vSheet <
    // 0.5` half of such a triangle was drawn as a sloping wall from the lake down to it (12-10).
    if (vDepth <= 0.0 && vSheet < 0.999) discard;
    const float depthShade = (vSheet > 0.5) ? max(vDepth, 0.3) : vDepth;
    // Vistas de depuracion (`HARUKA_TERRAIN_V5_DEBUG`, uMisc.z), las mismas que el terreno: 4 = color
    // por ZANCADA, 1 = por nivel. Existen por los "bloques negros en la linea del mar" (captura,
    // 18-09): si se colorean, son nodos de agua; si no, no es el agua.
    const int dbg = int(uMisc.z);
    if (dbg == 4) { fragColor = vec4(fract(vec3(0.31, 0.57, 0.83) * float(vWStride * 3 + 1)), 1.0); return; }
    if (dbg == 1) { fragColor = vec4(fract(vec3(0.31, 0.57, 0.83) * float(vWLevel + 1)), 1.0); return; }
    const vec3 wp = vFragPos - uCenter.xyz - uCenterLo.xyz;
    fragColor = harukaOceanShade(wp, vFragPos, normalize(vNormal), normalize(uLightDir.xyz),
                                 vec3(1.0), vec3(0.10, 0.13, 0.18), depthShade, vFoam,
                                 harukaTerrainFacet(length(vFragPos)));   // mismo fundido que el suelo
}
