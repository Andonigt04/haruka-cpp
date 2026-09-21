/**
 * @file terrain_strata.h
 * @brief Gemelo CPU de `assets/shaders/lib/terrain_strata.glsl`. El suelo como COLUMNA.
 *
 * ⚠️ SE ESCRIBIÓ A LA VEZ QUE EL .GLSL, A PROPÓSITO. El patrón de este motor es que toda función
 * que describa el terreno exista en los dos lados con las MISMAS cifras y el MISMO orden de
 * operaciones (ver `terrain_detail.h` ↔ `terrain_detail.glsl`), porque la alternativa —escribir el
 * shader y "ya haremos el de física"— es exactamente cómo se acumulan las discrepancias entre lo
 * que se ve y lo que se pisa. Aquí importa más que en ningún sitio: en cuanto se pueda CAVAR, el
 * espesor del manto decide si la pala saca arena o roca, y eso lo tiene que responder igual el
 * fragmento que dibuja y el sistema que excava.
 *
 * CUALQUIER CAMBIO EN UNA CONSTANTE VA EN LOS DOS FICHEROS A LA VEZ.
 *
 * Qué NO vive aquí: el estado dinámico. Nieve, fango y humedad cambian en minutos, no se hornean y
 * no son un estrato — modulan el resultado desde fuera (`uWet`). La columna es lo estable.
 */
#pragma once

#include <algorithm>

namespace Haruka { namespace Planet {

/// Gemelo de `smoothstep` de GLSL. Explícito para no depender de qué trae cada compilador.
inline float strataSmoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/**
 * @brief ESPESOR de la cobertura suelta (m) sobre el lecho. Gemelo de `harukaCoverThickness`.
 *
 * El sedimento no se agarra a una pendiente: resbala. El ángulo de reposo de la arena seca ronda
 * los 34° (slope01 ≈ 0.17) y de ahí a 0.35 la cobertura se anula — es la razón física por la que un
 * cortado se ve de roca, y sustituye a la banda `slope` que el material `rock` usaba para fingirlo.
 *
 * @param slope01  1 - dot(normal, up): 0 llano, 1 vertical.
 * @param humid01  humedad [0,1].
 * @param elevKm   cota sobre el nivel del mar (km).
 */
inline float coverThickness(float slope01, float humid01, float elevKm) {
    const float repose = 1.0f - strataSmoothstep(0.17f, 0.35f, slope01);
    const float soil   = 0.35f + (1.0f - 0.35f) * std::clamp(humid01, 0.0f, 1.0f);
    const float basin  = 1.0f + 1.6f * (1.0f - strataSmoothstep(-0.05f, 0.25f, elevKm));
    return 2.5f * repose * soil * basin;
}

/**
 * @brief Peso de la COBERTURA sobre el LECHO a una profundidad. Gemelo de `harukaCoverWeight`.
 *  1 = solo cobertura · 0 = solo lecho.
 */
inline float coverWeight(float depthM, float thickness) {
    if (thickness <= 0.0f) return 0.0f;
    const float blend = std::max(thickness * 0.30f, 0.05f);
    return 1.0f - strataSmoothstep(thickness - blend, thickness, depthM);
}

}} // namespace Haruka::Planet
