/**
 * @file terrain_strata.glsl
 * @brief EL SUELO ES UNA COLUMNA, no una lista de candidatos.
 *
 * ── POR QUÉ EXISTE ─────────────────────────────────────────────────────────────────────────────
 * Hasta ahora el material de superficie salía de un CONCURSO: cada material declaraba bandas de
 * clima y el shader promediaba a todos los que pesaran algo. Eso tiene tres fallos que no se
 * arreglan afinando pesos:
 *   · Un material sin bandas pesa 1 en TODO el planeta (los defaults son rango completo), así que
 *     basta con olvidarse de acotar uno para teñir el mundo entero con su color.
 *   · El color final es una MEDIA. Con siete materiales sueltos salía (0.30, 0.38, 0.41) — un velo
 *     gris-verde-azulado sobre el planeta, con los azules del mar metidos hasta en el desierto.
 *   · La textura se la lleva el de mayor `score`, y con prioridades empatadas decide el ORDEN DE
 *     DECLARACIÓN. Reordenar la escena cambiaba el planeta.
 *
 * El modelo de columna no tiene ninguno de los tres, porque no hay nada que promediar: hay un LECHO
 * y encima una COBERTURA con un ESPESOR, y la superficie es lo que asome.
 *
 * ── VOLUMÉTRICO, PERO NO UNA TEXTURA 3D ────────────────────────────────────────────────────────
 * ⚠️ La tentación es guardar esto en un volumen. No cabe: una corteza de 1 km sobre un planeta de
 * 6 371 km a 1 m³ son ~5·10¹⁷ voxels. Y no hace falta, porque es exactamente el problema que el
 * terreno ya resolvió con una FUNCIÓN — bake lento de baja resolución para lo que varía en
 * kilómetros, ruido procedural para lo que varía en metros, la misma expresión en CPU y GPU. Los
 * estratos siguen ese patrón: `harukaStrataAt(dir, profundidad)` se EVALÚA, no se almacena.
 *
 * El tiempo tampoco es una dimensión de esta función. Nieve, fango y humedad son estado DINÁMICO
 * que modula el resultado (`uWet`), no un estrato: cambian en minutos y no se pueden hornear.
 *
 * ⚠️ GEMELO EXACTO de `core/planet/terrain_strata.h`. Si divergen, el suelo que se cava deja de ser
 * el que se ve — que es la clase de discrepancia que ya costó una sesión entera con el agua.
 */
#ifndef HARUKA_TERRAIN_STRATA_GLSL
#define HARUKA_TERRAIN_STRATA_GLSL

/**
 * @brief ESPESOR de la cobertura suelta (m) sobre el lecho, en un punto.
 *
 * No es una regla de arte: es por qué un cortado se ve de roca y un llano de arena. El sedimento no
 * se agarra a una pendiente — resbala. El ángulo de reposo de la arena seca está en ~34°, y por
 * encima de él la cobertura tiende a cero. Eso es lo que la banda `slope: [0.35, 1]` de `rock`
 * estaba imitando a mano, y aquí sale solo.
 *
 * Tres factores, y cada uno responde a algo físico:
 *   · PENDIENTE — el que manda. `slope01` es 1 - dot(n, up): 0 llano, ~0.29 a 45°.
 *     El reposo (~34°) cae en 0.17, y de ahí a 0.35 la cobertura se va a cero.
 *   · HUMEDAD  — el suelo vivo genera y retiene material. Un desierto rocoso tiene menos manto que
 *     una pradera, aunque los dos estén llanos.
 *   · CUENCA   — por debajo del nivel del mar el sedimento se acumula (no hay a dónde escurrir), y
 *     por eso la plataforma y la playa son de arena aunque el lecho sea roca.
 *
 * @param slope01  1 - dot(normal, up) — 0 llano, 1 vertical.
 * @param humid01  humedad [0,1].
 * @param elevKm   cota sobre el nivel del mar, en km.
 * @return metros de cobertura. 0 = roca desnuda.
 */
float harukaCoverThickness(float slope01, float humid01, float elevKm) {
    // Reposo: 1 en el llano, 0 pasada la pendiente en la que el sedimento resbala.
    float repose = 1.0 - smoothstep(0.17, 0.35, slope01);
    // Suelo vivo: de 0,35 m en roca desnuda a 1,0 en pradera húmeda.
    float soil   = mix(0.35, 1.0, clamp(humid01, 0.0, 1.0));
    // Cuenca: el sedimento se acumula bajo el nivel del mar y en la costa.
    float basin  = 1.0 + 1.6 * (1.0 - smoothstep(-0.05, 0.25, elevKm));
    return 2.5 * repose * soil * basin;   // ~2,5 m en llano templado; ~9 m en una cuenca costera
}

/**
 * @brief Peso de la COBERTURA sobre el LECHO a una profundidad dada.
 *
 * 1 = solo cobertura · 0 = solo lecho. La transición no es un corte: los últimos centímetros de un
 * manto son cobertura mezclada con fragmentos del lecho, y ese degradado es lo que evita que el
 * borde de un cortado sea una línea recortada con tijera.
 *
 * @param depthM     profundidad bajo la superficie (m). 0 = superficie.
 * @param thickness  salida de `harukaCoverThickness`.
 */
float harukaCoverWeight(float depthM, float thickness) {
    if (thickness <= 0.0) return 0.0;
    // El degradado ocupa el 30 % final del manto, con un mínimo de 5 cm para que un manto muy fino
    // no se convierta en un corte duro.
    float blend = max(thickness * 0.30, 0.05);
    return 1.0 - smoothstep(thickness - blend, thickness, depthM);
}

#endif // HARUKA_TERRAIN_STRATA_GLSL
