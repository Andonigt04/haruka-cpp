#ifndef HARUKA_AERIAL_GLSL
#define HARUKA_AERIAL_GLSL
// ── PERSPECTIVA AÉREA: el aire entre el ojo y lo que se mira ────────────────────────────────────
//
// No existía. El terreno a 50-125 km se pintaba con el contraste de a 50 m, y las nubes a 100 km
// con el blanco de encima de tu cabeza: el horizonte era una raya dura (marrón contra azul) y la
// capa de nubes vista de canto se leía como tiras cortadas. Reportado mirando ("las nubes se
// entrecortan y el horizonte está cortado por clima y terreno").
//
// UNA función para terreno, props y nubes, para que el aire sea el MISMO en los tres. El color del
// aire es el del propio cielo en esa dirección (`harukaSkyBase`, la misma paleta que dibuja el domo),
// así que lo lejano se funde con el fondo en vez de con un gris inventado. Extinción exponencial con
// una sola escala `L` (metros de visibilidad), que el motor saca del clima: aire seco ~60 km,
// húmedo ~18 km, lloviendo mucho menos.
//
// `aerial`: x = 1/L (por metro, a NIVEL DEL MAR) · y = día [0,1] (harukaSkyDay en CPU) ·
//           z = altitud del ojo (m) · w = escala de altura del aire H (m). Ver abajo.
#include "lib/sky_palette.glsl"

/** @brief Transmitancia del aire a `distM` metros. */
float harukaAerialT(float distM, vec4 aerial) {
    return exp(-max(distM, 0.0) * aerial.x);
}

/** @brief Color visto tras `distM` metros de aire mirando con elevación `t` (dot(dir, up)), con el
 *  fragmento a la MISMA altitud que el ojo (props, cosas cercanas). */
vec3 harukaAerial(vec3 col, float distM, float t, vec4 aerial) {
    const float T   = harukaAerialT(distM, aerial);
    const vec3  air = harukaSkyBase(max(t, 0.0), aerial.y);   // bajo el horizonte: color de horizonte
    return mix(air, col, T);
}

// ⚠️ LA EXTINCIÓN VA POR EL AIRE ATRAVESADO, NO POR LOS METROS. Con `exp(−dist/L)` a secas, desde
// 250 km el planeta entero salía al 1,5 % de su color (e^{−250/60}) y desde 2 000 km a cero: TODO
// del color del aire bajo el horizonte, que es blanquecino — "en órbita está todo en blanco". El
// rayo desde órbita cruza ~10 km de aire, no 250. El aire decae con la altura (`e^{−h/H}`), y a lo
// largo de un tramo recto con la altura variando linealmente el camino EQUIVALENTE a nivel del mar
// tiene forma cerrada: `D·H/(hB−hA)·(e^{−hA/H} − e^{−hB/H})`. Desde órbita al nadir da ~`H` (unos km)
// sea cual sea la altitud; a ras de suelo da `D`, o sea lo de siempre.
// `aerial.z` = altitud del OJO sobre la esfera (m) · `aerial.w` = escala de altura H (m); 0 = plano.
/** @brief Metros de aire A NIVEL DEL MAR equivalentes a un tramo recto de `distM` entre las
 *  altitudes `hA` y `hB` (m), con escala de altura `H`. Con `H <= 0` es el camino plano. */
float harukaAirPath(float distM, float hA, float hB, float H) {
    if (H <= 0.0) return max(distM, 0.0);
    hA = max(hA, 0.0); hB = max(hB, 0.0);
    const float dh = hB - hA;
    if (abs(dh) < 1.0) return max(distM, 0.0) * exp(-hA / H);
    return max(distM, 0.0) * H / dh * (exp(-hA / H) - exp(-hB / H));
}
/** @brief Ídem `harukaAerial` con el fragmento a la altitud `hFragM`: el aire atravesado es el real. */
vec3 harukaAerialAlt(vec3 col, float distM, float t, float hFragM, vec4 aerial) {
    const float path = harukaAirPath(distM, aerial.z, hFragM, aerial.w);
    const float T    = exp(-path * aerial.x);
    const vec3  air  = harukaSkyBase(max(t, 0.0), aerial.y);
    return mix(air, col, T);
}
#endif
