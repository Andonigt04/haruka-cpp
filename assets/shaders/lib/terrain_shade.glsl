#ifndef HARUKA_TERRAIN_SHADE_GLSL
#define HARUKA_TERRAIN_SHADE_GLSL
/**
 * @file terrain_shade.glsl
 * @brief El ALBEDO del terreno: selección de material por clima + triplanar + normal map.
 *
 * ── POR QUÉ ESTO ES UNA LIBRERÍA Y NO UNA COPIA ─────────────────────────────────────────────────
 *
 * ⚠️ Este bloque vivía SOLO dentro de `biome.frag`. El pase de nodos (v5) necesita exactamente el
 * mismo color, y copiarlo habría sido la sexta copia divergida de un shader en este proyecto —
 * las cinco del toon ya costaron caro una vez. Peor aún aquí: si el nodo y el clipmap divergen en
 * color, la transición entre los dos se ve como una costura, que es justo lo que el v5 venía a
 * quitar.
 *
 * Los samplers van como PARÁMETROS, no como uniforms globales, para que cada pase ate lo que tenga
 * y la librería no imponga bindings. Los mapas opcionales se apagan con sus flags.
 *
 * Requiere que el incluyente traiga antes `lib/terrain_material.glsl` (`harukaSelectMaterial`).
 */

vec2 harukaEquirectUVDir(vec3 dir) {
    vec3 d = normalize(dir);
    return vec2(0.5 + atan(d.z, d.x) * 0.1591549,
                0.5 - asin(clamp(d.y, -1.0, 1.0)) * 0.3183099);
}

// Triplanar sobre una CAPA del array: los tres planos pesados por la normal elevada a 4.
vec3 harukaTriplanarArr(sampler2DArray tex, float layer, vec3 wp, vec3 n, float s) {
    vec3 bw = pow(abs(n), vec3(4.0));
    bw /= max(bw.x + bw.y + bw.z, 1e-6);
    return texture(tex, vec3(wp.zy * s, layer)).rgb * bw.x
         + texture(tex, vec3(wp.xz * s, layer)).rgb * bw.y
         + texture(tex, vec3(wp.xy * s, layer)).rgb * bw.z;
}

// Normal map en "whiteout blend": se suman las desviaciones TANGENCIALES de las tres proyecciones,
// no los vectores ya girados. Mezclar normales de mundo las promedia hacia la normal geométrica y
// aplana el relieve justo en las caras diagonales, que son media esfera.
vec3 harukaTriplanarArrNrm(sampler2DArray tex, float layer, vec3 wp, vec3 n, float s) {
    vec3 bw = pow(abs(n), vec3(4.0));
    bw /= max(bw.x + bw.y + bw.z, 1e-6);
    vec3 tx = texture(tex, vec3(wp.zy * s, layer)).xyz * 2.0 - 1.0;
    vec3 ty = texture(tex, vec3(wp.xz * s, layer)).xyz * 2.0 - 1.0;
    vec3 tz = texture(tex, vec3(wp.xy * s, layer)).xyz * 2.0 - 1.0;
    return vec3(0.0, tx.y, tx.x) * sign(n.x) * bw.x
         + vec3(ty.x, 0.0, ty.y) * sign(n.y) * bw.y
         + vec3(tz.x, tz.y, 0.0) * sign(n.z) * bw.z;
}

/**
 * @brief Color del terreno en un punto. `n` se MODIFICA con el normal map (el llamador debe
 *        reiluminar con la normal que salga).
 *
 * @param fragP      posición relativa a la cámara (metros).
 * @param texAnchor  ancla del patrón, ya reducida módulo el tile en doubles por la CPU.
 * @param elevKm     elevación BASE (sin detalle) en km — decide la costa y la roca por altura.
 * @param tiling     METROS POR TILE.
 * @param lod        desvanecido del color/arena por distancia; 0 = textura neutra.
 * @param lodNrm     desvanecido del normal map (más agresivo: es lo caro).
 */
vec3 harukaTerrainAlbedo(sampler2DArray albedoTex, sampler2DArray normalTex,
                         sampler2D macroTex, sampler2D biomeTex, sampler2D zoneTex,
                         bool hasMacro, bool hasBiome, bool hasZone,
                         vec3 fragP, vec3 texAnchor, inout vec3 n, vec3 up,
                         float elevKm, float tempC, float humid,
                         float tiling, int shoreLayer, float lod, float lodNrm,
                         out vec3 outBiomeCol, out int outMatIdx,
                         // ⚠️ SOLO PARA DEPURAR: la muestra triplanar CRUDA y el peso con que entra
                         // en el color final. Sirve para partir "el suelo sale liso" en dos: si `tex`
                         // ya viene plano, el fallo esta en el MUESTREO (array sin atar, capa mala,
                         // escala absurda); si viene con detalle, el fallo esta en la MEZCLA.
                         out vec3 outRawTex, out float outTexW)
{
    vec2 bUV = harukaEquirectUVDir(up);
    vec3 biomeCol = hasBiome ? texture(biomeTex, bUV).rgb : vec3(0.5);
    float slope = 1.0 - dot(n, up);

    vec3 zoneRGB = hasZone ? texture(zoneTex, bUV).rgb * 255.0 : vec3(0.0);
    vec3  tint; float grainAmt, detailAmt; int tile; vec4 matColor; int matIdx;
    int   tileBed; float coverW;   // capa del LECHO y cuánto manto la tapa (ver terrain_strata.h)
    harukaSelectMaterial(humid, tempC, slope, elevKm, 0.0, zoneRGB, hasZone,
                         tint, grainAmt, detailAmt, tile, tileBed, coverW, matColor, matIdx);
    biomeCol = mix(biomeCol, matColor.rgb, matColor.a);
    // El llamador los necesita despues (vistas de depuracion, cobertura de props), y recomputarlos
    // fuera significaria volver a recorrer la tabla de materiales: salen por aqui.
    outBiomeCol = biomeCol;
    outRawTex = vec3(0.5); outTexW = 0.0;   // por si se sale antes
    outMatIdx   = matIdx;

    // ⚠️ El patrón se ancla al PLANETA, no a la cámara, y NO se reconstruye la posición planetaria
    // (`fragP - uCenter`): esa resta es entre dos floats de ~6,37e6 y queda cuantizada a un ulp de
    // 0,76 m POR PÍXEL, con lo que las derivadas de la GPU eligen un mip arbitrario y el suelo sale
    // sal y pimienta. `fragP` es relativo a la cámara y el ancla ya viene reducida en doubles.
    vec3 wp = fragP + texAnchor;
    float tileScale = 1.0 / max(tiling, 0.01);

    // ⚠️ LUMINANCIA MEDIA AUTORIZADA A 0.5: `tools/gen_terrain_textures.py` normaliza cada PNG a esa
    // media exacta con estos MISMOS pesos, así que `tex / kTexMean` tiene media 1.0 y multiplicar por
    // él es neutro en promedio. Antes el número era 0.45, medido a ojo sobre unos ficheros concretos.
    const float kTexMean = 0.5;
    const vec3  kLumaW   = vec3(0.2126, 0.7152, 0.0722);
    vec3 tex;
    if (lod < 0.01) {
        tex = vec3(kTexMean);
    } else if (coverW > 0.99 || tileBed == tile) {
        tex = (tile < 0) ? vec3(kTexMean)
                         : harukaTriplanarArr(albedoTex, float(tile), wp, n, tileScale);
    } else if (coverW < 0.01) {
        tex = (tileBed < 0) ? vec3(kTexMean)
                            : harukaTriplanarArr(albedoTex, float(tileBed), wp, n, tileScale);
    } else {
        vec3 texC = (tile    < 0) ? vec3(kTexMean)
                                  : harukaTriplanarArr(albedoTex, float(tile),    wp, n, tileScale);
        vec3 texB = (tileBed < 0) ? vec3(kTexMean)
                                  : harukaTriplanarArr(albedoTex, float(tileBed), wp, n, tileScale);
        tex = mix(texB, texC, coverW);
    }

    float grain = mix(1.0, dot(tex, kLumaW) / kTexMean, grainAmt);
    float texW = clamp(grainAmt * 0.55, 0.0, 0.75) * lod;
    outRawTex = tex; outTexW = texW;
    vec3 col = mix(biomeCol, biomeCol * tex / kTexMean, texW) * tint * clamp(grain, 0.75, 1.25);
    if (hasMacro) col *= 0.85 + 0.30 * texture(macroTex, bUV).r * step(0.0, elevKm);

    // La ARENA DE ORILLA sale de la elevación BASE (banda −30 m … +12 m), no de la total: la costa
    // la define el mapa base, no el ruido, que es relieve local y no estructura de continente.
    float shoreF = smoothstep(-0.030, -0.002, elevKm) * (1.0 - smoothstep(0.0, 0.012, elevKm));
    float sandW = shoreF * (1.0 - smoothstep(0.45, 0.7, slope));
    if (lod > 0.01 && sandW > 0.001 && shoreLayer >= 0)
        col = mix(col, harukaTriplanarArr(albedoTex, float(shoreLayer), wp, n, tileScale * 2.0), sandW);

    vec3 nrmDelta = (tile < 0 || lodNrm < 0.01)
                  ? vec3(0.0)
                  : harukaTriplanarArrNrm(normalTex, float(tile), wp, n, tileScale);
    if (dot(nrmDelta, nrmDelta) > 1e-8) {
        vec3 dTan = nrmDelta - dot(nrmDelta, n) * n;
        n = normalize(n + 0.35 * detailAmt * (0.5 + 0.5 * lodNrm) * dTan);
    }
    return col;
}

#endif // HARUKA_TERRAIN_SHADE_GLSL
