/**
 * @file aerial.glsl
 * @brief LA PERSPECTIVA AEREA POR FISICA: lo lejano se ve a traves del aire que hay en medio.
 *
 * Es la MISMA integral que pinta el cielo (`lib/atmosphere.glsl`), cortada en el objeto: lo de detras
 * llega atenuado por la transmitancia del tramo y se le suma la luz que ese aire dispersa hacia el
 * ojo. Por eso una montaña lejana se funde EXACTAMENTE con el cielo de su direccion, al atardecer se
 * tiñe de naranja y de noche no brilla.
 *
 * ⚠️ AQUI HABIA un `mix(colorDelCielo, col, exp(-d/L))` con el color del cielo sacado de la paleta de
 * cuatro colores y una visibilidad L elegida (60 km seco, 18 km humedo). Andoni (17-10): "la
 * atmosfera los colores estan harcodeados no por atmosfera".
 *
 * Usa la luz del frame (`lib/sky_light.glsl`, binding 31) y la tabla de dispersion multiple (binding 20).
 */
#ifndef HARUKA_AERIAL_GLSL
#define HARUKA_AERIAL_GLSL

#include "lib/sky_light.glsl"
#include "lib/atmosphere.glsl"

/// Pasos de la integral para el aire de en medio: es por pixel de TODO el suelo, asi que menos que el
/// cielo (que tiene que resolver el halo del Sol y el crepusculo).
const int   HARUKA_AERIAL_VIEW_STEPS  = 6;
const int   HARUKA_AERIAL_LIGHT_STEPS = 3;
/// Por debajo de esto el aire de en medio no se ve (T > 0,998 en el azul): se ahorra la integral.
const float HARUKA_AERIAL_MIN_M = 150.0;

/**
 * @brief Color de PANTALLA de algo a `fragRel` (relativo al OJO, m), visto a traves del aire.
 * @param displayCol  su color de pantalla ya iluminado (lo que se veria sin aire en medio).
 */
vec3 harukaAerialSky(vec3 displayCol, vec3 fragRel) {
    const float R = uSkyPlanet.w;
    const float d = length(fragRel);
    if (R <= 0.0 || d < HARUKA_AERIAL_MIN_M) return displayCol;
    vec3 T;
    const vec3 S = harukaAtmoInscatterN(-uSkyPlanet.xyz, fragRel / d, d, R,
                                        uSkySunDir.xyz, uSkySunE.rgb, uSkyMoonDir.xyz, uSkyMoonE.rgb,
                                        HARUKA_AERIAL_VIEW_STEPS, HARUKA_AERIAL_LIGHT_STEPS, T);
    return harukaDisplay(harukaLinear(displayCol) * T + S);
}

/// Lo mismo en LINEAL (para quien compone su luz en lineal antes de llevarla a pantalla: las nubes).
vec3 harukaAerialSkyLinear(vec3 linearCol, vec3 fragRel) {
    const float R = uSkyPlanet.w;
    const float d = length(fragRel);
    if (R <= 0.0 || d < HARUKA_AERIAL_MIN_M) return linearCol;
    vec3 T;
    const vec3 S = harukaAtmoInscatterN(-uSkyPlanet.xyz, fragRel / d, d, R,
                                        uSkySunDir.xyz, uSkySunE.rgb, uSkyMoonDir.xyz, uSkyMoonE.rgb,
                                        HARUKA_AERIAL_VIEW_STEPS, HARUKA_AERIAL_LIGHT_STEPS, T);
    return linearCol * T + S;
}

/**
 * @brief EL AIRE POR VÉRTICE: transmitancia `T` y luz dispersada `S` (lineales) hasta `rel` (relativo
 *        al OJO, m). Para lo que se dibuja con miles de fragmentos por objeto lejano: el aire cambia en
 *        kilómetros, no de un píxel al siguiente, y una nube a 80 km son ~200 px con un puñado de
 *        vértices. Medido: los cúmulos de malla integraban el aire POR FRAGMENTO (con solapamiento
 *        entre bultos) y costaban 4,2 ms de GPU. Se aplica en el fragment con `harukaAerialApply`.
 */
void harukaAerialTS(vec3 rel, out vec3 T, out vec3 S) {
    T = vec3(1.0); S = vec3(0.0);
    const float R = uSkyPlanet.w;
    const float d = length(rel);
    if (R <= 0.0 || d < HARUKA_AERIAL_MIN_M) return;
    S = harukaAtmoInscatterN(-uSkyPlanet.xyz, rel / d, d, R,
                             uSkySunDir.xyz, uSkySunE.rgb, uSkyMoonDir.xyz, uSkyMoonE.rgb,
                             HARUKA_AERIAL_VIEW_STEPS, HARUKA_AERIAL_LIGHT_STEPS, T);
}
/// Color de PANTALLA visto a través del aire `T`, `S` (de `harukaAerialTS`, interpolados).
vec3 harukaAerialApply(vec3 displayCol, vec3 T, vec3 S) {
    if (S == vec3(0.0) && T == vec3(1.0)) return displayCol;   // de cerca: sin ida y vuelta por lineal
    return harukaDisplay(harukaLinear(displayCol) * T + S);
}

#endif // HARUKA_AERIAL_GLSL
