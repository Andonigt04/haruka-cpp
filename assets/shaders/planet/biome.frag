#version 460 core
in vec3 vNorm; in vec3 vFragPos; in vec3 vColor; in vec2 vUv; in vec3 vClimate;
layout(std140, binding = 0) uniform SimplePlanetUBO {
    mat4 uMVP; vec4 uCenter; vec4 uLightDir; vec4 uLightColor; vec4 uAmbient; vec4 uExtra;
    vec4 uDebug; // x = vista de depuración: 0=normal, 1=elev, 2=zonas, 3=bioma, 4=temp, 5=humedad
};
layout(binding = 1) uniform sampler2D uSandAlbedo;
layout(binding = 2) uniform sampler2D uSandNormal;
layout(binding = 3) uniform sampler2D uGrassAlbedo;
layout(binding = 4) uniform sampler2D uGrassNormal;
layout(binding = 5) uniform sampler2D uLandAlbedo;
layout(binding = 6) uniform sampler2D uLandNormal;
layout(binding = 7) uniform sampler2D uRockAlbedo;
layout(binding = 8) uniform sampler2D uRockNormal;
layout(binding = 10) uniform sampler2D uMacroVar;
layout(binding = 11) uniform sampler2D uBiomeMap;
// ARRAYS de terreno: una capa por material. El `tile` de la tabla es el ÍNDICE DE CAPA, así que
// elegir textura deja de ser una cadena de `if` sobre samplers (GLSL no los indexa dinámicamente).
layout(binding = 12) uniform sampler2DArray uTerrainAlbedo;
layout(binding = 13) uniform sampler2DArray uTerrainNormal;
// MAPA DE ZONAS equirectangular (paleta). uExtra.z > 0.5 = existe.
layout(binding = 14) uniform sampler2D uZoneMap;

#include "lib/terrain_material.glsl"
out vec4 fragColor;

vec2 equirectUV(vec3 dir) {
    vec3 d = normalize(dir);
    return vec2(0.5 + atan(d.z, d.x) * 0.1591549,
                0.5 - asin(clamp(d.y, -1.0, 1.0)) * 0.3183099);
}

vec3 triplanar(sampler2D tex, vec3 wp, vec3 n, float s) {
    vec3 bw = pow(abs(n), vec3(4.0));
    bw /= max(bw.x + bw.y + bw.z, 1e-6);
    return texture(tex, wp.zy * s).rgb * bw.x
         + texture(tex, wp.xz * s).rgb * bw.y
         + texture(tex, wp.xy * s).rgb * bw.z;
}

// Triplanar de NORMAL MAP en "whiteout blend": se suman las desviaciones tangenciales de las tres
// proyecciones, no los vectores ya girados. Mezclar normales de mundo las promedia hacia la normal
// geometrica y aplana el relieve justo en las caras diagonales, que son media esfera.
// Los `u*Normal` estaban DECLARADOS y no se muestreaban nunca: media VRAM de terreno sin usar.
// Triplanar sobre una CAPA del array: idéntico al de sampler2D, con la capa en la 3ª coordenada.
vec3 triplanarArr(sampler2DArray tex, float layer, vec3 wp, vec3 n, float s) {
    vec3 bw = pow(abs(n), vec3(4.0));
    bw /= max(bw.x + bw.y + bw.z, 1e-6);
    return texture(tex, vec3(wp.zy * s, layer)).rgb * bw.x
         + texture(tex, vec3(wp.xz * s, layer)).rgb * bw.y
         + texture(tex, vec3(wp.xy * s, layer)).rgb * bw.z;
}

vec3 triplanarArrNrm(sampler2DArray tex, float layer, vec3 wp, vec3 n, float s) {
    vec3 bw = pow(abs(n), vec3(4.0));
    bw /= max(bw.x + bw.y + bw.z, 1e-6);
    vec3 tx = texture(tex, vec3(wp.zy * s, layer)).xyz * 2.0 - 1.0;
    vec3 ty = texture(tex, vec3(wp.xz * s, layer)).xyz * 2.0 - 1.0;
    vec3 tz = texture(tex, vec3(wp.xy * s, layer)).xyz * 2.0 - 1.0;
    return vec3(0.0, tx.y, tx.x) * sign(n.x) * bw.x
         + vec3(ty.x, 0.0, ty.y) * sign(n.y) * bw.y
         + vec3(tz.x, tz.y, 0.0) * sign(n.z) * bw.z;
}

vec3 triplanarNrm(sampler2D tex, vec3 wp, vec3 n, float s) {
    vec3 bw = pow(abs(n), vec3(4.0));
    bw /= max(bw.x + bw.y + bw.z, 1e-6);
    vec3 tx = texture(tex, wp.zy * s).xyz * 2.0 - 1.0;
    vec3 ty = texture(tex, wp.xz * s).xyz * 2.0 - 1.0;
    vec3 tz = texture(tex, wp.xy * s).xyz * 2.0 - 1.0;
    return vec3(0.0, tx.y, tx.x) * sign(n.x) * bw.x
         + vec3(ty.x, 0.0, ty.y) * sign(n.y) * bw.y
         + vec3(tz.x, tz.y, 0.0) * sign(n.z) * bw.z;
}

// Rampa de color para las vistas de depuración: azul → cian → verde → amarillo → rojo.
vec3 heatmap(float t) {
    t = clamp(t, 0.0, 1.0);
    vec3 c = mix(vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 1.0), smoothstep(0.00, 0.25, t));
    c = mix(c, vec3(0.0, 1.0, 0.0), smoothstep(0.25, 0.50, t));
    c = mix(c, vec3(1.0, 1.0, 0.0), smoothstep(0.50, 0.75, t));
    c = mix(c, vec3(1.0, 0.0, 0.0), smoothstep(0.75, 1.00, t));
    return c;
}

// Color CATEGÓRICO por capa de textura (tile). Fijo para que "hierba" sea siempre la misma
// mancha verde en cualquier planeta; capas sin asignar de la paleta no se usan.
vec3 layerColor(int l) {
    vec3 pal[16] = vec3[16](
        vec3(0.95, 0.80, 0.35),   // 0 arena
        vec3(0.30, 0.65, 0.25),   // 1 hierba
        vec3(0.55, 0.40, 0.25),   // 2 tierra
        vec3(0.55, 0.55, 0.55),   // 3 roca
        vec3(0.70, 0.30, 0.20),   // 4
        vec3(0.80, 0.60, 0.90),   // 5
        vec3(0.20, 0.60, 0.70),   // 6
        vec3(0.90, 0.45, 0.55),   // 7
        vec3(0.45, 0.55, 0.35),   // 8
        vec3(0.85, 0.70, 0.50),   // 9
        vec3(0.55, 0.35, 0.60),   // 10
        vec3(0.65, 0.70, 0.30),   // 11
        vec3(0.80, 0.50, 0.30),   // 12
        vec3(0.40, 0.40, 0.55),   // 13
        vec3(0.75, 0.65, 0.40),   // 14
        vec3(0.60, 0.80, 0.80)    // 15
    );
    return pal[clamp(l, 0, 15)];
}

// Color de un MATERIAL en las vistas de capas. El matIdx es la posición en `surface.materials`
// (NO el tile: incluye agua/hielo, que no tienen textura). Se prefiere el color DECLARADO por el
// material (agua azul, hielo casi blanco, arena…) y sin él se cae a la paleta categórica.
vec3 matDebugColor(int i) {
    if (i < 0) return vec3(0.10, 0.10, 0.12);
    vec4 mc = uMat[i].e;
    return mc.a > 0.001 ? mc.rgb : layerColor(i);
}

void main() {
    vec3 n = normalize(vNorm);
    float diff = max(dot(n, normalize(uLightDir.xyz)), 0.0);
    float tiling = uExtra.y;

    // Radial direction (from planet center to surface point)
    vec3 up = normalize(vFragPos - uCenter.xyz);
    float elev = vClimate.x; // km

    // Sample biome map for classification + color
    vec2 bUV = equirectUV(up);
    vec4 biomeSample = texture(uBiomeMap, bUV);
    vec3 biomeCol = biomeSample.rgb;
    float H = biomeSample.a; // biome index / humidity [0,1]

    // Rock override by slope
    float slope = 1.0 - dot(n, up);
    float rock = smoothstep(0.55, 0.80, slope) + smoothstep(2.0, 5.0, elev);

    // --- SELECCION DE MATERIAL: se recorre la tabla -------------------------------------
    // La humedad y la TEMPERATURA llegan por vertice (`vClimate.yz`) y hasta ahora se ignoraban:
    // el shader leia la humedad del alfa del mapa horneado y no miraba la temperatura en absoluto,
    // asi que no podia distinguir una taiga de una pradera.
    float humid = clamp(vClimate.z, 0.0, 1.0);
    float tempC = vClimate.y;

    // El mapa de zonas se lee en el MISMO UV equirectangular que el mapa de biomas: los dos
    // describen el mismo planeta y si divergieran, el color diría una cosa y el material otra.
    // SIN ruido: la geometría (sampleHeight, nearest sin jitter) usa el mismo texel, así que el
    // material y el borde del agua coinciden. Perturbar aquí con ruido descasaba el material del
    // terreno (un texel ≈ 20 km a 2048) y pintaba agua azul sobre tierra en la franja de la costa.
    bool hasZoneMap = uExtra.z > 0.5;
    vec3 zoneRGB = hasZoneMap ? texture(uZoneMap, bUV).rgb * 255.0 : vec3(0.0);

    vec3  tint; float grainAmt, detailAmt; int tile; vec4 matColor; int matIdx;
    harukaSelectMaterial(humid, tempC, slope, zoneRGB, hasZoneMap,
                         tint, grainAmt, detailAmt, tile, matColor, matIdx);
    // El color PROPIO del material sustituye al del bioma según su peso. Con peso 0 (o sin color
    // declarado) manda el clima, que es el comportamiento de siempre.
    biomeCol = mix(biomeCol, matColor.rgb, matColor.a);

    // `tiling` es METROS POR TILE. Estaba usándose al revés (`vFragPos * tiling` con un scale de
    // 0.5 encima) → un tile cada 2 cm: muy por debajo del píxel a cualquier distancia, así que la
    // textura se promediaba a gris plano y no aportaba ni grano ni relieve. De ahí que el terreno
    // se viera liso por mucha resolución que tuvieran los PNG.
    // Anclado al PLANETA, no a la cámara: vFragPos es relativo a la cámara (aPos + uCenter), así que
    // usarlo como coordenada de textura hacía que el patrón se deslizara con el jugador. Restando el
    // centro queda la posición planetaria (≈6.37e6, precisión ~0.8 m) — estable al moverse.
    vec3 wp = vFragPos - uCenter.xyz;
    float tileScale = 1.0 / max(tiling, 0.01);
    // Una sola llamada: el material dice QUE CAPA, no que sampler. tile < 0 = sin textura (hielo,
    // sal, lava vidriada): luminancia neutra -> grano ~1 y superficie lisa.
    vec3 tex = (tile < 0) ? vec3(0.45) : triplanarArr(uTerrainAlbedo, float(tile), wp, n, tileScale);

    // Macro variation brightness modulation
    vec4 macro = texture(uMacroVar, bUV);
    // El PNG aporta GRANO, no color: el color es el del bioma, que sale del clima. `grainAmt`
    // decide cuanto se nota por material (el hielo casi nada, la roca mas que nadie).
    float grain = mix(1.0, dot(tex, vec3(0.333)) / 0.45, grainAmt);
    // El ALBEDO SE VE, no solo su luminancia: se mezcla el COLOR de la textura con el del bioma
    // para que el patron (hierba, arena, roca) sea visible. Antes el color plano del material
    // lo tapaba y la textura solo aportaba grano, que es como "no tener texturas".
    // `tex * 2.0` compensa el brillo medio de los PNG (~0.45); el peso sale de `grainAmt` (los
    // materiales sin textura quedan intactos: tex = 0.45 constante y mix ≈ identidad).
    float texW = clamp(grainAmt * 0.55, 0.0, 0.75);
    vec3 col = mix(biomeCol, biomeCol * tex * 2.0, texW) * tint * clamp(grain, 0.75, 1.25);
    // La variación de macro solo en TIERRA: en el mar (liso, sin tiles) su patrón de manchas se
    // veía como círculos grises. step(0,elev) = 1 en tierra, 0 en mar.
    col *= 0.85 + 0.30 * macro.r * step(0.0, elev);

    // Sand overlay at shoreline
    float ocean = elev < 0.0 ? 1.0 : 0.0;
    float shoreF = ocean * (1.0 - smoothstep(-0.2, 0.0, elev));
    float sandW = shoreF * (1.0 - smoothstep(0.45, 0.7, slope));
    col = mix(col, triplanar(uSandAlbedo, wp, n, tileScale * 1.4), sandW);

    // RELIEVE del material: la desviacion de su normal map. Solo la parte tangencial (la componente
    // a lo largo de n no inclina nada y si desnormaliza) y con fuerza baja: con sombreado cel un
    // relieve fuerte pica el terminador y saca manchas oscuras.
    vec3 nrmDelta = (tile < 0) ? vec3(0.0)
                               : triplanarArrNrm(uTerrainNormal, float(tile), wp, n, tileScale);
    if (dot(nrmDelta, nrmDelta) > 1e-8) {
        vec3 dTan = nrmDelta - dot(nrmDelta, n) * n;
        n = normalize(n + 0.35 * detailAmt * dTan);
        diff = max(dot(n, normalize(uLightDir.xyz)), 0.0);   // reiluminar con la normal nueva
    }

    // Luz del SOL + CIELO, MISMA respuesta que el agua: `sunD` satura antes (clamp(diff*1.6), igual
    // que water.frag) y con multiplicador más alto, más un destello especular tenue para que el
    // terreno "atrape" el sol igual que el agua. Antes el sol era lineal (×1.3*diff) y a ángulos
    // rasantes la cara de día salía apagada: terreno oscuro y "el sol apenas lo toca" en
    // comparación con el agua, que sí brillaba.
    vec3 sky = vec3(0.09, 0.14, 0.22);
    vec3 L = normalize(uLightDir.xyz);
    vec3 V = -vFragPos;
    V = length(V) > 1e-6 ? normalize(V) : vec3(0.0, 0.0, 1.0);
    vec3 H = normalize(L + V);
    float sunD = clamp(diff * 1.6, 0.0, 1.0);
    float sheen = pow(max(dot(n, H), 0.0), 24.0) * 0.30;
    col = col * (uAmbient.xyz + sky * (0.35 + 0.65 * sunD) + uLightColor.xyz * sunD * 1.6)
        + uLightColor.xyz * sheen;

    // VISTAS DE DEPURACIÓN del editor: colorean el planeta para "ver cómo funciona" el terreno.
    // Se aplican DESPUÉS de la iluminación y SIN texturas: lo que importa es el dato, no el grano.
    int dbg = int(uDebug.x);
    if (dbg == 1) {                       // elevación (km): ±5 km normalizado
        col = heatmap(elev * 0.1 + 0.5);
    } else if (dbg == 2) {                // zonas del autor (paleta del mapa)
        col = hasZoneMap ? zoneRGB / 255.0 : vec3(0.3, 0.3, 0.35);
    } else if (dbg == 3) {                // bioma clasificado (mapa horneado)
        col = biomeCol;
    } else if (dbg == 4) {                // temperatura (°C): ±40 °C normalizado
        col = heatmap(tempC / 80.0 + 0.5);
    } else if (dbg == 5) {                // humedad [0,1]
        col = heatmap(humid);
    } else if (dbg == 6) {                // CAPAS: TODAS a la vez, cada material con su color
        col = matDebugColor(matIdx);
    } else if (dbg >= 10) {               // CAPA i: solo donde manda ese material (máscara).
        // El índice es la POSICIÓN en `surface.materials` (0=agua, 1=arena…), no el tile: así
        // los materiales sin textura (agua, hielo) también se pueden aislar.
        int layer = dbg - 10;
        col = (matIdx == layer) ? matDebugColor(layer) : vec3(0.05, 0.06, 0.09);
    }
    fragColor = vec4(col, 1.0);
}
