#version 460 core
#extension GL_GOOGLE_include_directive : require
// ── EL SUELO CERCANO, DIBUJADO DESDE LA GEOMETRÍA DE LA COLISIÓN ────────────────────────────────
//
// Gemelo de la COLA de `clipmap.tese`, con una diferencia que es el motivo entero de que exista:
// aquí la POSICIÓN no se calcula, LLEGA. Los vértices vienen de la física, ya colocados, y son los
// mismos que Jolt colisiona y los mismos que dibuja el alambre de depuración.
//
// Por qué no basta con afinar el tese: los cuatro nodos de cada quad ya coinciden exactamente, pero
// un quad no es plano y hay que partirlo en dos triángulos. Jolt parte por `(i,j)→(i+1,j+1)`; el
// teselador de la GPU parte por donde quiera, porque el spec de OpenGL NO lo fija para
// `layout(quads, ...)`. Medido sobre el terreno real, la diferencia en el centro del quad son 2-4 cm
// bajo los pies. Mientras el teselador esté en el bucle esa disparidad solo se puede acotar.
//
// De la función de terreno se sigue usando el GRADIENTE, y solo para la normal: la iluminación tiene
// que describir la misma superficie que el clipmap de al lado o la costura a 256 m se vería como un
// cambio de sombreado. La ALTURA que devuelve se descarta a propósito — la altura ya está en el
// vértice, y volver a calcularla sería reintroducir la segunda evaluación que esto elimina.
layout(location = 0) in vec3 aPosRelAnchor;   // posición de mundo RELATIVA al ancla (float, ~±362 m)

layout(location = 0) out vec3 vNorm; layout(location = 1) out vec3 vFragPos;
layout(location = 2) out vec3 vColor; layout(location = 3) out vec2 vUv;
layout(location = 4) out vec3 vClimate;

layout(std140, binding = 0) uniform SimplePlanetUBO {
    mat4 uMVP; vec4 uCenter; vec4 uLightDir; vec4 uLightColor; vec4 uAmbient; vec4 uExtra; vec4 uDebug;
    vec4 uTexAnchor;
};
layout(std140, binding = 13) uniform ClipParams {
    vec4 uClipOrigin; vec4 uClipTanU; vec4 uClipTanV; vec4 uClipCover;
};
// Ancla del anillo, RELATIVA AL OJO, y su vertical. ⚠️ Binding 22: el 14 lo ocupa `uZoneMap`
// (un sampler) en `biome.frag`, y declarar ahí un UBO da "mismatching definitions" al enlazar —
// el pipeline se creaba en FALLO y no se dibujaba nada. Se recalcula por frame en double y solo entonces
// baja a float: en coordenadas de mundo (~6,37e6 m) un float tiene ~0,5 m de resolución y el suelo
// temblaría medio metro.
layout(std140, binding = 22) uniform NearRingParams {
    vec4 uAnchorRelEye;   // xyz = ancla − ojo
    vec4 uAnchorUp;       // xyz = vertical del marco anclado
};
layout(binding = 15) uniform sampler2DArray uBaseField;
layout(binding = 16) uniform sampler2D uHeightTex;

#include "lib/terrain_detail.glsl"
#include "lib/cube_face.glsl"

// Bilineal A MANO, idéntica a la de `clipmap.tese`: el filtrado del hardware usa pesos de 8 bits en
// varias GPU y eso bastaría para que este parche y el clipmap de al lado difieran en centímetros.
vec3 sampleBase(vec3 dir) {
    int f; vec2 uv;
    harukaDirToCubeFace(dir, f, uv);
    int N = textureSize(uBaseField, 0).x - 1;
    vec2 fxy = (uv * 0.5 + 0.5) * float(N);
    ivec2 i0 = clamp(ivec2(floor(fxy)), ivec2(0), ivec2(N - 1));
    vec2 t = fxy - vec2(i0);
    vec3 h00 = texelFetch(uBaseField, ivec3(i0 + ivec2(0,0), f), 0).rgb;
    vec3 h10 = texelFetch(uBaseField, ivec3(i0 + ivec2(1,0), f), 0).rgb;
    vec3 h01 = texelFetch(uBaseField, ivec3(i0 + ivec2(0,1), f), 0).rgb;
    vec3 h11 = texelFetch(uBaseField, ivec3(i0 + ivec2(1,1), f), 0).rgb;
    vec3 a = h00 + (h10 - h00) * t.x;
    vec3 b = h01 + (h11 - h01) * t.x;
    return a + (b - a) * t.y;
}

void main() {
    float R = uExtra.w;
    // Del ancla al marco del planeta. `uCenter.xyz` es el centro del planeta relativo al ojo, así que
    // restarlo lleva el punto a coordenadas planetarias, que es donde vive `dir`.
    vec3 posRelEye  = aPosRelAnchor + uAnchorRelEye.xyz;
    vec3 posPlanet  = posRelEye - uCenter.xyz;
    vec3 dir        = normalize(posPlanet);

    vec3  fld   = sampleBase(dir);
    float baseH = uDebug.z > 0.5
                ? harukaSampleHeightField(uHeightTex, textureSize(uHeightTex, 0), harukaEquirectUV(dir))
                : fld.x;
    float baseR = R + baseH;
    float att   = harukaSeaLevelAttenuation(baseH);

    // `triM` = 4 m FIJO, y no es una simplificación arbitraria: el anillo cercano llega a ±256 m, o
    // sea `rad` ≤ 362 m, y `max(rad·0.002, 4.0)` vale 4,0 en todo ese rango. Por lo mismo, el anillo
    // de mezcla del tese (`uClipCover.y`, ~70 % del semi-lado del clipmap) queda muy por fuera y su
    // rama nunca se activa aquí: una sola evaluación, no tres.
    vec3 grad;
    harukaTerrainDetailGrad(dir, baseR, 4.0, grad);   // ⚠️ la ALTURA se descarta: ya está en el vértice
    grad *= att;

    vec3 t1 = normalize(abs(dir.y) < 0.99 ? cross(dir, vec3(0,1,0)) : cross(dir, vec3(1,0,0)));
    vec3 t2 = cross(dir, t1);
    vNorm = normalize(dir - t1 * dot(grad, t1) - t2 * dot(grad, t2));

    // ⚠️ vFragPos ES MI POSICIÓN, no `dir*(baseR+h)`. Recalcularla desde la función volvería a poner
    // el suelo dibujado donde la función dice en vez de donde la colisión está — que es el bug.
    vFragPos = posRelEye;

    // Coordenadas tangentes respecto al ancla, para las UV. Mismo `·0.01` que el tese.
    vec2 loc = vec2(dot(aPosRelAnchor, uClipTanU.xyz), dot(aPosRelAnchor, uClipTanV.xyz));
    vUv    = loc * 0.01;
    vColor = vec3(1.0);
    // Elevación de la BASE sin el detalle: `biome.frag` decide con esto dónde está la costa, y la
    // costa la define el mapa base, no el ruido. (Ver la nota larga de `clipmap.tese`.)
    vClimate = vec3(baseH * 0.001, fld.y, fld.z);

    gl_Position = uMVP * vec4(posRelEye, 1.0);
}
