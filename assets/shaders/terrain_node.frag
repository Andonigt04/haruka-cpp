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
layout(location = 6) flat in int vStride;
layout(location = 8) flat in int vKUp;   // vista 9: profundidad de caida a ancestro
layout(location = 9) in float vDispM;   // vista 10: disparidad dibujado-dato, en metros
layout(location = 10) in float vMapF;   // vista 12: mapa leido (0 propio, 1 padre, 2 abuelo; fraccion = rampa)
layout(location = 7) flat in int vFace;

// ⚠️ EL COLOR SALE DE LA MISMA LIBRERIA QUE EL CLIPMAP, NO DE UNA COPIA. Si el nodo y el clipmap
// divergen en color, la transicion entre los dos se ve como una costura — justo lo que el v5 venia
// a quitar. Ver `lib/terrain_shade.glsl`.
#include "lib/terrain_material.glsl"
#include "lib/aerial.glsl"            // harukaAerial: perspectiva aérea (la misma que props y nubes)   // harukaSelectMaterial + su UBO (binding 12)
#include "lib/terrain_shade.glsl"      // harukaTerrainAlbedo
#include "lib/terrain_detail.glsl"     // harukaTerrainDetail — para la vista 11 (ver vs pisar)

layout(binding = 10) uniform sampler2D      uMacroVar;
layout(binding = 11) uniform sampler2D      uBiomeMap;
layout(binding = 12) uniform sampler2DArray uTerrainAlbedo;
layout(binding = 13) uniform sampler2DArray uTerrainNormal;
layout(binding = 14) uniform sampler2D      uZoneMap;

layout(std140, binding = 0) uniform NodeDraw {
    // ⚠️ ESTE BLOQUE ES GEMELO EXACTO DE `terrain_node.vert` Y DE `DrawUBO`. Un campo de mas o de
    // menos aqui no da error de compilacion: desplaza TODO lo que viene detras y el shader lee
    // basura de otro campo. `uCenterLo` se anadio el 2026-08-25 y hay que llevarlo en los tres.
    mat4  uMVP; vec4 uCenter; vec4 uCenterLo; vec4 uLod; ivec4 uGrid; ivec4 uEdgeUnused;
    vec4  uMisc;
    // x = metros por tile · y = capa de arena de orilla · z = bits de mapas presentes
    // (1 macro | 2 biomas | 4 zonas) · w = 1 si se sombrea de verdad, 0 = luz plana de geometría
    vec4  uShade;
    vec4  uTexAnchor;   // ancla planetaria del patrón, reducida módulo el tile en doubles por CPU
    vec4  uLightDir;
    vec4  uAerial;      // x = 1/L extinción por metro · y = día (ver lib/aerial.glsl)
    vec4  uStrideRamp;  // rampa del mapa por zancada: x = radio zancada 1 · y = fracción de rampa ·
                        // z = vertexPx/errorPx · w = celda fina de colisión (ver nodeStrideMapF)
};

layout(location = 0) out vec4 fragColor;

// ── EL RECORTE DEL CAMPO VOLUMÉTRICO (bocas de cueva, lo picado) ────────────────────────────────
//
// El heightfield no sabe de aire debajo: la boca de una cueva es un AGUJERO en el suelo, y aquí se
// hace no dibujándolo. `uVoxCut` es una ventana R8 de ±320 m alrededor del pie de la cámara, cada
// téxel = `VoxWorld::surfaceCut(dir)` calculado por la CPU con la MISMA función que usa la física
// y la malla de paredes: lo que aquí desaparece es exactamente donde el campo dice aire. Borde
// suave (3 m) en el mapa y umbral a 0,5: un escalón por téxel dejaría un filo dentado.
// `uVoxCutSpace` lleva una posición RELATIVA AL OJO (como `vFragPos`) a [0,1]² de la ventana;
// `uVoxCutInfo.x` = 1 si la ventana vale (0 = no hay campo cargado: no se recorta nada).
layout(binding = 17) uniform sampler2D uVoxCut;
layout(std140, binding = 3) uniform VoxCut {
    mat4 uVoxCutSpace;
    vec4 uVoxCutInfo;
};

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
 *   4 = por STRIDE del nodo, sin iluminar. Si un pincho cae justo en una frontera de color,
 *       viene del stride por nodo o de su cosido; si cae en medio de un color, no.
 *  10 = DISPARIDAD dibujado-dato en metros: verde <5cm · amarillo 0,5 m · rojo >=2 m · azul sin dato.
 *   9 = CAIDA A ANCESTRO: verde = datos propios · amarillo->rojo = k niveles por encima.
 *   6 = LA NORMAL como color (sin iluminar): separa un pliegue de GEOMETRIA de uno de la NORMAL.
 *   5 = ATRIBUCION EXACTA, para leer con `readPixels` (no para mirar). R = nivel x 8, G = cara x 40,
 *       B = 200 + stride x 8 (marca de "aqui hay terreno" Y el stride, para atribuir). La 1 pinta por nivel pero con una paleta ciclica de
 *       8, asi que el nivel 7 y el 15 salen del mismo color: sirve para mirar y NO para atribuir.
 *       Con esta, cada agujero se puede achacar a una frontera concreta — de nivel o de CARA.
 */
vec3 debugLevelColor(int lv) {
    const vec3 pal[8] = vec3[8](vec3(1,0,0), vec3(1,0.5,0), vec3(1,1,0), vec3(0,1,0),
                                vec3(0,1,1), vec3(0,0.4,1), vec3(0.6,0,1), vec3(1,0,1));
    return pal[lv & 7];
}

void main() {
    if (uVoxCutInfo.x > 0.5) {
        const vec2 cuv = (uVoxCutSpace * vec4(vFragPos, 1.0)).xy;
        const bool dentro = cuv.x > 0.0 && cuv.x < 1.0 && cuv.y > 0.0 && cuv.y < 1.0;
        const float cut = dentro ? texture(uVoxCut, cuv).r : 0.0;
        // `HARUKA_VOX_DEBUG=1` (uVoxCutInfo.y): en vez de recortar, PINTA: rojo = recorte, verde =
        // dentro de la ventana. Distingue "la ventana no llega aquí" de "llega y no recorta".
        if (uVoxCutInfo.y > 1.5) {          // =2: la uv como color (R = u, G = v, B = recorte)
            fragColor = vec4(clamp(cuv, 0.0, 1.0), cut, 1.0); return;
        } else if (uVoxCutInfo.y > 0.5) {
            if (cut > 0.05) { fragColor = vec4(1.0, 0.0, 0.0, 1.0); return; }
            if (dentro)     { fragColor = vec4(0.1, 0.6, 0.1, 1.0); return; }
        } else if (cut > 0.5) {
            discard;
        }
    }
    const int dbg = int(uMisc.z);
    if (dbg == 1) { fragColor = vec4(debugLevelColor(vLevel), 1.0); return; }
    if (dbg == 5) {   // atribucion: se LEE, no se mira. Ver la nota de arriba.
        // B lleva DOS cosas: sigue siendo la marca de "aqui hay terreno" (>=200 siempre) y ademas
        // codifica el stride, para poder achacar un agujero que caiga DENTRO de un nivel — donde el
        // escalon entre niveles no puede explicarlo y el sospechoso es el cosido o el stride.
        fragColor = vec4(float(vLevel) * 8.0 / 255.0, float(vFace) * 40.0 / 255.0,
                         (200.0 + float(vStride) * 8.0) / 255.0, 1.0);
        return;
    }
    if (dbg == 2) { fragColor = vec4(1.0); return; }
    // 6 = LA NORMAL como color, sin iluminacion. Es lo que separa "la superficie tiene pliegues"
    // de "la normal los tiene": si aqui hay aristas duras, el fallo es de sombreado; si sale suave,
    // los pliegues son geometria de verdad. Ninguna vista anterior podia contestar eso.
    if (dbg == 6) { fragColor = vec4(normalize(vNormal) * 0.5 + 0.5, 1.0); return; }
    if (dbg == 3) { fragColor = vec4(vec3(pow(gl_FragCoord.z, 0.15)), 1.0); return; }
    if (dbg == 4) { fragColor = vec4(debugLevelColor(vStride * 3), 1.0); return; }
    // ── 12 = EL NIVEL ABSOLUTO DEL MAPA QUE SE LEE, en gris: `nivel del nodo − mapa`, 0,1 + 0,08·(A − 6)
    // (A = 17 = el mapa mas fino de la Tierra, casi blanco; A = 6, casi negro). Con el corte continuo
    // (`nodeStrideMapF`) tiene que ser un DEGRADADO sin escalones: en una frontera de nivel el hijo
    // lee su mapa 3,1 y el padre su 2,1, que es el MISMO mapa absoluto. ⚠️ La primera version pintaba
    // el indice RELATIVO (0..4) y Andoni vio "escalones de gris" que eran solo eso: la resta del nivel.
    // Un escalon de gris AQUI si es un escalon del corte.
    if (dbg == 12) { fragColor = vec4(vec3(clamp(0.1 + 0.08 * (float(vLevel) - vMapF - 6.0), 0.0, 1.0)), 1.0); return; }
    // ── 13 = DISTANCIA al ojo, en gris (0..2 000 m). Para que el banco sepa a que distancia esta cada
    // pixel de la vista 12 y compare el mapa leido con la ley (`nodeStrideMapF`).
    if (dbg == 13) { fragColor = vec4(vec3(clamp(length(vFragPos) / 2000.0, 0.0, 1.0)), 1.0); return; }
    // ── 11 = MAPA DE "LO QUE SE VE MENOS LO QUE SE PISA", EN METROS ─────────────────────────────
    //
    // ⚠️ ESTA ES LA VISTA QUE FALTABA, Y LA RAZON DE QUE FALTARA ES EL PATRON DE SIEMPRE: toda la
    // caza de esta disparidad se hizo con sondas NUMERICAS que dan centimetros, mientras en pantalla
    // se ve medio metro. La vista 10 (dibujado contra el texel del nodo) no vale para esto: compara
    // contra el dato del propio vertice, donde coinciden por construccion, y por eso salia verde.
    //
    // Aqui la referencia es la SUPERFICIE DE COLISION: `harukaTerrainDetail` con el corte que usa
    // `world_system_provider.h`, o sea `terrainTriM(radM) = max(radM*0,002 , 0,5)` con `radM` medido
    // desde el punto del planeta bajo el ojo. Verde <5 cm · amarillo 0,25 m · rojo >= 1 m.
    //
    // ⚠️ EN DOUBLE, Y NO ES OPCIONAL: un float a 6,37e6 tiene un ulp de 0,5 m, que es justo lo que se
    // quiere medir. `vFragPos` si puede ser float porque es relativo al OJO (escala de km).
    //
    // ⚠️ Y `radM` POR LA CUERDA, no con `acos`: un nodo de nivel 17 subtiende 6e-6 rad, `cos` de eso
    // es 1,0 exacto en float y `acos` devolvia 0. La sonda del banco se comio ese fallo hasta que la
    // contraprueba de perturbar la pendiente no cambio nada.
    // ── 14 = LA VISTA 11 EN NUMEROS: |dibujado − pisado| en 16 bits (R alto, G bajo, ±64 m, B = 255 =
    // hay terreno), para que el BANCO lo lea y lo afirme (`testFrameDrawnVsBaked`). Misma referencia
    // que la 11 —el campo con el corte de la colision desde el ancla bajo el ojo— pero CON EL BAKE,
    // compuesto como `nodeFillHeights`/`terrain_node.comp`: base + detalle atenuado, sin hundir
    // tierra bajo el mar. Sin el bake la 11 en el juego compara contra un planeta sin continentes.
    // La 11 (colores) y la 14 (16 bits) comparten referencia. ⚠️ La 11 comparaba SIN bake
    // (`detail(dir, R)`): en el juego, con continentes, pintaba magenta por el propio bake.
    if (dbg == 14 || dbg == 11) {
        const float  baseH  = vClimate.x * 1000.0;
        const dvec3  rel    = dvec3(vFragPos) - dvec3(uCenter.xyz) - dvec3(uCenterLo.xyz);
        const double r      = length(rel);
        const dvec3  dir    = rel / r;
        const dvec3  anchor = normalize(-(dvec3(uCenter.xyz) + dvec3(uCenterLo.xyz)));
        // Gemelos de TERRAIN_TRIM_SLOPE y TERRAIN_TRIM_FLOOR (`terrain_lod.h`).
        const double radM = double(uMisc.x) * length(dir - anchor);
        const float  cut  = float(max(radM * 0.002LF, 0.5LF));
        double href = double(harukaTerrainDetail(dir, double(uMisc.x) + double(baseH), cut));
        if (uMisc.w > 0.5 || uMisc.y > 0.5) {   // con bake: la composicion de `terrain_node.comp`
            href *= double(harukaSeaLevelAttenuation(baseH));
            if (baseH > 0.0 && href < -double(baseH)) href = -double(baseH);
            href += double(baseH);
        }
        const double err  = r - (double(uMisc.x) + href);      // + = se DIBUJA por encima de lo que se pisa
        if (dbg == 14) {
            const double fs = 64.0LF;
            const double t  = clamp((err + fs) / (2.0LF * fs), 0.0LF, 1.0LF);   // 0,5 = error cero
            const uint   q  = uint(t * 65535.0LF + 0.5LF);
            fragColor = vec4(float(q >> 8u) / 255.0, float(q & 255u) / 255.0, 1.0, 1.0);
            return;
        }
        const float  a    = float(abs(err));
        vec3 col = (a < 0.05) ? vec3(0.0, 0.6, 0.1)
                 : (a < 0.25) ? mix(vec3(0.0,0.6,0.1), vec3(0.9,0.9,0.0), (a - 0.05) / 0.20)
                 : (a < 1.00) ? mix(vec3(0.9,0.9,0.0), vec3(1.0,0.0,0.0), (a - 0.25) / 0.75)
                              : vec3(1.0, 0.0, 1.0);           // magenta: fuera de escala
        // El SIGNO en el azul: oscuro = el terreno se dibuja por DEBAJO de lo que se pisa, que es
        // justo como Andoni describe el sintoma ("el terreno esta por debajo").
        col.b = mix(col.b, 0.35, err < 0.0 ? 1.0 : 0.0);
        fragColor = vec4(col, 1.0);
        return;
    }
    // ⚠️ VISTA 9 = CAIDA A ANCESTRO. Verde: el nodo dibuja con SUS datos (k=0), que es el caso sano.
    // Del amarillo al rojo, cuantos niveles por encima esta leyendo. Existe porque "escalon dentro de
    // un nodo" tiene causas distintas segun si ese nodo cayo o no, y a ojo son indistinguibles: un
    // nodo caido dibuja la superficie de su ancestro sobre su propia huella, asi que se ve terreno
    // normal, solo que mas basto de lo que le toca.
    // ⚠️ VISTA 10 = MAPA DE DISPARIDAD, la que faltaba. Verde oscuro: lo dibujado coincide con el
    // dato (< 5 cm). Amarillo: medio metro. Rojo: dos metros o mas. Azul: el nodo lee de un ancestro
    // y no hay texel fino con que comparar. La ESCALA IMPORTA — sin ella "se ve rojo" no dice cuanto.
    if (dbg == 10) {
        if (vDispM < -0.5) { fragColor = vec4(0.15, 0.25, 0.85, 1.0); return; }   // sin dato fino
        const float a = abs(vDispM);
        const float t = clamp(a / 2.0, 0.0, 1.0);
        vec3 c = (a < 0.05) ? vec3(0.05, 0.35, 0.10)
                            : mix(vec3(0.95, 0.90, 0.15), vec3(0.90, 0.08, 0.05), t);
        fragColor = vec4(c, 1.0);
        return;
    }
    if (dbg == 9) {
        if (vKUp <= 0) { fragColor = vec4(0.10, 0.65, 0.20, 1.0); return; }   // sano
        const float t = clamp(float(vKUp) / 7.0, 0.0, 1.0);                    // 7 = el mas hondo medido
        fragColor = vec4(mix(vec3(0.95, 0.90, 0.10), vec3(0.90, 0.10, 0.05), t), 1.0);
        return;
    }
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
    vec3 rawTex; float texW;           // vistas 7 y 8: la muestra cruda y su peso
    vec3 col = harukaTerrainAlbedo(uTerrainAlbedo, uTerrainNormal, uMacroVar, uBiomeMap, uZoneMap,
                                   (maps & 1) != 0, (maps & 2) != 0, (maps & 4) != 0,
                                   vFragPos, uTexAnchor.xyz, n, normalize(vUp),
                                   vClimate.x, vClimate.y, clamp(vClimate.z, 0.0, 1.0),
                                   uShade.x, int(uShade.y), lod, lodNrm, ignoredCol, ignoredMat,
                                   rawTex, texW);

    // ⚠️ VISTAS PARA PARTIR "EL SUELO SALE LISO" EN DOS. Medido: el detalle local del suelo esta al
    // nivel del CIELO (que es un degradado, o sea liso) en los DOS backends, cuando con `grain ~1` y
    // `lod = 1` a los pies el albedo deberia aportar el 55 % del color.
    //   · vista 7 = la muestra triplanar CRUDA. Si sale plana, el fallo es el MUESTREO.
    //   · vista 8 = el PESO con que entra (`texW`), en gris. Si sale negro, la mezcla la anula.
    if (dbg == 7) { fragColor = vec4(rawTex, 1.0); return; }
    if (dbg == 8) { fragColor = vec4(vec3(texW), 1.0); return; }

    // `harukaTerrainAlbedo` MODIFICA `n` con el normal map, asi que la iluminacion va despues.
    const float d = max(dot(n, normalize(uLightDir.xyz)), 0.0);
    vec3 lit = col * (0.25 + 0.75 * d);
    // El aire de en medio: lo lejano se funde con el cielo de esa dirección (lib/aerial.glsl).
    // ⚠️ Con la ALTITUD del fragmento (`vClimate.x`, km): desde orbita el rayo cruza unos km de
    // aire, no cientos (ver lib/aerial.glsl: "en orbita esta todo en blanco").
    lit = harukaAerialAlt(lit, dist, dot(vFragPos / max(dist, 1.0), normalize(vUp)), vClimate.x * 1000.0, uAerial);
    fragColor = vec4(lit, 1.0);
}
