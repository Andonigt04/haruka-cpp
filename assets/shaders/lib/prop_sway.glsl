/**
 * @file prop_sway.glsl
 * @brief El balanceo por VIENTO de la vegetación: el MISMO para props y para la capa masiva, o la
 *        hierba de un brote y el árbol de al lado se moverían con vientos distintos.
 *
 * El viento es el DEL CLIMA (el mismo que mueve nubes y lluvia): con tormenta encima el follaje se
 * dobla hacia donde van las nubes, y en calma apenas se mueve.
 */
#ifndef HARUKA_PROP_SWAY_GLSL
#define HARUKA_PROP_SWAY_GLSL

/**
 * @param wind   viento del clima (m/s).
 * @param time   reloj (s).
 * @param phase  fase propia (por posición), para que no vayan todas a la vez.
 * @param h      altura local del vértice (m, ya recortada): el pie no se mueve.
 * @return desplazamiento (m).
 */
vec3 harukaPropSway(vec3 wind, float time, float phase, float h) {
    vec3  wdir  = (dot(wind, wind) > 1e-4) ? normalize(wind) : vec3(1.0, 0.0, 0.0);
    float wspd  = clamp(length(wind) / 12.0, 0.15, 1.6);   // 12 m/s ≈ ondeo pleno
    float sway  = sin(time * 1.7 + phase) + 0.35 * sin(time * 3.3 + phase * 1.7);
    float flap  = cos(time * 1.4 + phase);                  // vaivén perpendicular (no un péndulo plano)
    vec3  wside = normalize(cross(vec3(0.0, 1.0, 0.0), wdir) + vec3(1e-5));
    return h * wspd * (wdir * (0.035 + 0.05 * sway) + wside * 0.03 * flap);
}

#endif // HARUKA_PROP_SWAY_GLSL
