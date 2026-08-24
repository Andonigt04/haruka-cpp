#version 460 core
#extension GL_GOOGLE_include_directive : require
/**
 * @file terrain_node.frag
 * @brief Fragmento del nodo del quadtree (v5, F3). Deliberadamente MÍNIMO.
 *
 * ⚠️ NO HAY SPHERE-TRACE AQUÍ, y ésa es toda la noticia. `biome.frag` re-evalúa el detalle por píxel
 * dentro de `reanchorToFine` (hasta 16 pasos, cada uno una `harukaTerrainDetail` completa) porque la
 * geometría de la malla base no lleva relieve y hay que fingirlo. Aquí la geometría SÍ lo lleva: la
 * puso el nodo. El fragmento solo ilumina.
 *
 * El sombreado real (biomas, triplanar, materiales) es trabajo aparte y viene después: mezclarlo
 * ahora impediría comparar el coste de la GEOMETRÍA contra el camino que sustituye, que es lo que
 * este pase existe para medir.
 */
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vFragPos;
layout(location = 2) in float vHeight;
layout(location = 3) flat in int vLevel;
layout(location = 4) in vec3 vClimate;
layout(location = 5) in vec3 vUp;

// ⚠️ EL COLOR SALE DE LA MISMA LIBRERIA QUE EL CLIPMAP, NO DE UNA COPIA. Si el nodo y el clipmap
// divergen en color, la transicion entre los dos se ve como una costura — justo lo que el v5 venia
// a quitar. Ver `lib/terrain_shade.glsl`.
#include "lib/terrain_material.glsl"   // harukaSelectMaterial + su UBO (binding 12)
#include "lib/terrain_shade.glsl"      // harukaTerrainAlbedo

layout(binding = 10) uniform sampler2D      uMacroVar;
layout(binding = 11) uniform sampler2D      uBiomeMap;
layout(binding = 12) uniform sampler2DArray uTerrainAlbedo;
layout(binding = 13) uniform sampler2DArray uTerrainNormal;
layout(binding = 14) uniform sampler2D      uZoneMap;

layout(std140, binding = 0) uniform NodeDraw {
    mat4  uMVP; vec4 uCenter; ivec4 uNodeUnused; ivec4 uGrid; ivec4 uEdgeUnused; vec4 uMisc;
    // x = metros por tile · y = capa de arena de orilla · z = bits de mapas presentes
    // (1 macro | 2 biomas | 4 zonas) · w = 1 si se sombrea de verdad, 0 = luz plana de geometría
    vec4  uShade;
    vec4  uTexAnchor;   // ancla planetaria del patrón, reducida módulo el tile en doubles por CPU
    vec4  uLightDir;
};

layout(location = 0) out vec4 fragColor;

/**
 * Vistas de depuración (`HARUKA_TERRAIN_V5_DEBUG`, en uMisc.z). Existen porque "falta terreno" tiene
 * varias causas que se ven IGUAL en la imagen final, y cada una se arregla en un sitio distinto:
 *
 *   1 = PLANO por NIVEL de nodo, sin iluminar. Si el terreno lejano aparece aquí y no en la vista
 *       normal, el problema es la NORMAL (gradiente malo, NaN) y no la geometría.
 *   2 = BLANCO plano. Si aquí tampoco aparece, no llega ni el fragmento: es geometría, profundidad
 *       o recorte — no sombreado.
 *   3 = por PROFUNDIDAD (gl_FragCoord.z, reversed-Z: cerca=1, lejos=0). Delata si el lejano cae al
 *       0 exacto, que con la comparación GREATER contra un clear de 0 no pasaría el test.
 */
vec3 debugLevelColor(int lv) {
    const vec3 pal[8] = vec3[8](vec3(1,0,0), vec3(1,0.5,0), vec3(1,1,0), vec3(0,1,0),
                                vec3(0,1,1), vec3(0,0.4,1), vec3(0.6,0,1), vec3(1,0,1));
    return pal[lv & 7];
}

void main() {
    const int dbg = int(uMisc.z);
    if (dbg == 1) { fragColor = vec4(debugLevelColor(vLevel), 1.0); return; }
    if (dbg == 2) { fragColor = vec4(1.0); return; }
    if (dbg == 3) { fragColor = vec4(vec3(pow(gl_FragCoord.z, 0.15)), 1.0); return; }
    vec3 n = normalize(vNormal);

    // ⚠️ LA LUZ PLANA SE CONSERVA A PROPOSITO (uShade.w = 0). No es codigo muerto: es el lado A del
    // A/B con el que se mide lo que cuesta el sombreado de verdad. Sin un "antes" medible en la
    // MISMA ejecucion, la cifra del "despues" no significa nada.
    if (uShade.w < 0.5) {
        const vec3 L = normalize(vec3(0.4, 0.8, 0.3));
        float d = max(dot(n, L), 0.0);
        vec3 base = mix(vec3(0.22, 0.28, 0.18), vec3(0.55, 0.52, 0.45),
                        clamp(vHeight * 0.002 + 0.5, 0.0, 1.0));
        fragColor = vec4(base * (0.25 + 0.75 * d), 1.0);
        return;
    }

    const int maps = int(uShade.z);
    const float dist = length(vFragPos);
    // Los mismos desvanecidos por distancia que usa `biome.frag`: el color y la arena de orilla se
    // apagan a 3-12 km, y el normal map —que es lo caro— mucho antes, a 300-2500 m.
    const float lod    = 1.0 - smoothstep(3000.0, 12000.0, dist);
    const float lodNrm = 1.0 - smoothstep( 300.0,  2500.0, dist);

    vec3 ignoredCol; int ignoredMat;   // solo los usa `biome.frag`, para sus vistas de depuracion
    vec3 col = harukaTerrainAlbedo(uTerrainAlbedo, uTerrainNormal, uMacroVar, uBiomeMap, uZoneMap,
                                   (maps & 1) != 0, (maps & 2) != 0, (maps & 4) != 0,
                                   vFragPos, uTexAnchor.xyz, n, normalize(vUp),
                                   vClimate.x, vClimate.y, clamp(vClimate.z, 0.0, 1.0),
                                   uShade.x, int(uShade.y), lod, lodNrm, ignoredCol, ignoredMat);

    // `harukaTerrainAlbedo` MODIFICA `n` con el normal map, asi que la iluminacion va despues.
    const float d = max(dot(n, normalize(uLightDir.xyz)), 0.0);
    fragColor = vec4(col * (0.25 + 0.75 * d), 1.0);
}
