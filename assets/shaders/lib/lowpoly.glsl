/**
 * @file lowpoly.glsl
 * @brief El estilo LOW-POLY SIN COSTURAS, sin dependencias: lo usan mallas (toon_shade.glsl) y
 *        terreno (terrain_node.frag, planet/biome.frag), que no comparten UBO.
 *
 * La idea, en cuatro piezas:
 *   1. La luz DIRECTA se calcula con la normal de la CARA (derivadas de pantalla): cada triángulo
 *      tiene un tono — el look low-poly.
 *   2. La banda de luz es ANCHA (`HARUKA_FACET_BAND_*`): dos caras vecinas con normales parecidas
 *      dan tonos parecidos, así que entre ellas hay un cambio suave de valor, no una arista marcada.
 *      Estrechar la banda = más facetado y más duro.
 *   3. Donde la normal de autor contradice a la geometría (hojas, normales editadas) gana la de
 *      autor: ver `harukaLightingNormal`.
 *   4. Lo que NO es luz directa (rim, ambiente de cielo, selección de material, pesos triplanar)
 *      sigue con la normal SUAVE. Con la de cara, cada una de esas cosas cambia de golpe en cada
 *      arista — una costura por triángulo.
 *
 * MEMORIA: la normal de cara sale del fragment, así que una malla low-poly NO necesita duplicar
 * vértices para tener normales por cara (hoy PrimitiveShapes mete 24 vértices en un cubo para eso).
 */
#ifndef HARUKA_LOWPOLY_GLSL
#define HARUKA_LOWPOLY_GLSL

// --- Mandos globales del estilo (un solo sitio para retocar el look de TODO) ---------------------
#ifndef HARUKA_FACET_BAND_LO
#define HARUKA_FACET_BAND_LO -0.25
#endif
#ifndef HARUKA_FACET_BAND_HI
#define HARUKA_FACET_BAND_HI  0.65
#endif

// Terreno: 1 = facetado y SIN el relieve fino por píxel de biome.frag (sphere-trace de hasta 16
// evaluaciones de ruido por píxel). Ese relieve inventa detalle más fino que el triángulo, que es
// justo lo contrario de low-poly, y es la parte más cara del fragment del terreno. 0 = lo de antes.
#ifndef HARUKA_TERRAIN_LOWPOLY
#define HARUKA_TERRAIN_LOWPOLY 1
#endif
// De lejos los triángulos del terreno se hacen subpíxel y la normal de cara se vuelve ruido: se
// funde a la suave entre estas dos distancias (metros).
#ifndef HARUKA_TERRAIN_FACET_NEAR
#define HARUKA_TERRAIN_FACET_NEAR  6000.0
#endif
#ifndef HARUKA_TERRAIN_FACET_FAR
#define HARUKA_TERRAIN_FACET_FAR  20000.0
#endif

/**
 * @brief Normal geométrica de la cara a partir de la posición interpolada.
 *
 * Se orienta hacia el observador en vez de confiar en el signo del producto cruz: ese signo depende
 * de hacia dónde crece la Y de pantalla, que es distinta en GL y en Vulkan. Con backface culling
 * toda cara visible mira a la cámara, así que orientar por `V` es exacto.
 *
 * @param pos  posición del fragmento (mundo o cámara-relativa: solo importa la orientación).
 * @param V    vector hacia el ojo, normalizado, en el mismo espacio.
 */
vec3 harukaFaceNormal(vec3 pos, vec3 V) {
    vec3 n = cross(dFdx(pos), dFdy(pos));
    float l2 = dot(n, n);
    if (l2 < 1e-24) return V;                     // triángulo degenerado en pantalla
    n *= inversesqrt(l2);
    return dot(n, V) < 0.0 ? -n : n;
}

/**
 * @brief Normal de luz directa según `facet` (1 = cara, 0 = suave).
 *
 * ⚠️ LA NORMAL DE AUTOR MANDA CUANDO CONTRADICE A LA GEOMETRÍA. Hay mallas cuya normal NO es la de la
 * superficie a propósito: tarjetas de hojas con la normal hacia arriba (para que la copa se ilumine
 * como un volumen y no como cartas sueltas), quads de prueba, normales editadas. Facetar eso tira la
 * decisión del autor y la cambia por la de un plano que nadie quería ver. Así que el facetado se
 * desvanece cuando la cara y la normal suave se separan más de ~55° — en una malla low-poly normal
 * (vértices compartidos, normal media) la separación queda muy por debajo.
 *
 * La cara se orienta por la normal SUAVE, no solo por la vista: así una malla a dos caras se ilumina
 * igual que antes por detrás, y un `V` degenerado (cámara en el plano de la cara) no la voltea.
 */
vec3 harukaLightingNormal(vec3 smoothN, vec3 pos, vec3 V, float facet) {
    if (facet <= 0.0) return smoothN;
    vec3 f = harukaFaceNormal(pos, V);
    float agree = dot(f, smoothN);
    if (agree < 0.0) { f = -f; agree = -agree; }
    float w = clamp(facet, 0.0, 1.0) * smoothstep(0.30, 0.60, agree);
    return w >= 1.0 ? f : normalize(mix(smoothN, f, w));
}

/// Lleva a la normal de luz el DETALLE que un normal map haya añadido a la suave
/// (`mappedN - smoothN`), para que facetar no borre el relieve de la textura.
vec3 harukaCarryDetail(vec3 lightN, vec3 smoothN, vec3 mappedN) {
    return normalize(lightN + (mappedN - smoothN));
}

/**
 * @brief Respuesta difusa en [0,1]: la banda ancha del facetado, mezclada por `facet` con la
 *        respuesta que el shader usaba antes (`baseResponse`: Lambert, cel estrecho…).
 */
float harukaFacetBand(float baseResponse, float ndl, float facet) {
    return mix(baseResponse, smoothstep(HARUKA_FACET_BAND_LO, HARUKA_FACET_BAND_HI, ndl),
               clamp(facet, 0.0, 1.0));
}

/// Cuánto facetar el terreno a esta distancia (ver HARUKA_TERRAIN_FACET_*).
float harukaTerrainFacet(float dist) {
    return HARUKA_TERRAIN_LOWPOLY != 0
         ? 1.0 - smoothstep(HARUKA_TERRAIN_FACET_NEAR, HARUKA_TERRAIN_FACET_FAR, dist)
         : 0.0;
}

#endif // HARUKA_LOWPOLY_GLSL
