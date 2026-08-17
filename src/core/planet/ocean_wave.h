/**
 * @file ocean_wave.h
 * @brief Gemelo CPU de `assets/shaders/lib/ocean_wave.glsl`. El oleaje para quien no dibuja.
 *
 * ⚠️ EXISTE PORQUE SIN ÉL EL AGUA TIENE DOS FORMAS. El shader levanta olas de metro y medio y la
 * física sigue viendo un plano: una barca flota atravesada, un nadador sube y baja por una ola que
 * no existe para él, y el ahogamiento se decide contra una cota que no es la que se ve. Es la misma
 * clase de discrepancia que costó una sesión entera con el terreno (lo que se pisa contra lo que se
 * dibuja), y se evita de la única forma que se evita: escribiendo las dos caras a la vez, con las
 * MISMAS cifras y el MISMO orden de operaciones.
 *
 * CUALQUIER CAMBIO EN UNA CONSTANTE VA EN LOS DOS FICHEROS.
 *
 * Qué NO hace: no evalúa espuma ni normal. La física solo necesita la COTA de la superficie; la
 * normal y la rompiente son cosa del sombreado y pagarlas aquí sería trabajo para nadie.
 */
#pragma once

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

namespace Haruka { namespace Planet {

inline constexpr float  OCEAN_G     = 9.81f;
inline constexpr int    OCEAN_WAVES = 4;

/// (longitud de onda m, amplitud en aguas profundas m, dir.x, dir.y). Gemelo de `HARUKA_WAVE`.
inline constexpr float OCEAN_WAVE[OCEAN_WAVES][4] = {
    { 61.0f, 0.85f,  1.000f,  0.000f },
    { 37.0f, 0.45f,  0.766f,  0.643f },
    { 19.0f, 0.22f,  0.174f, -0.985f },
    {  8.7f, 0.09f, -0.500f,  0.866f },
};

/// Gemelo de `harukaShoalAmp`: ley de Green + límite de rompiente (la ola no crece, revienta).
inline float oceanShoalAmp(float depthM, float ampDeep) {
    const float d     = std::max(depthM, 0.05f);
    const float green = std::pow(std::clamp(50.0f / d, 1.0f, 40.0f), 0.25f);
    return std::min(ampDeep * green, 0.55f * depthM);
}

/**
 * @brief Cota de la superficie del agua sobre el reposo, en metros. Gemelo de `harukaGerstner`,
 *        pero devolviendo SOLO la componente vertical (a lo largo de `up`).
 *
 * ⚠️ El desplazamiento de Gerstner también es HORIZONTAL —es lo que afila la cresta—, así que la
 * superficie no es una función de la posición: un punto del plano puede estar bajo dos partes de la
 * ola. Para flotación y ahogamiento eso no importa (basta la altura en la vertical del punto) y
 * resolverlo bien exigiría invertir el desplazamiento, que es iterativo y caro. Se documenta el
 * límite en vez de esconderlo: con mar gruesa y una cresta muy inclinada, la cota que devuelve esto
 * puede quedarse hasta un `Q·amplitud` corta respecto a lo que se ve.
 *
 * @param wp      posición de la superficie en reposo, relativa al CENTRO del planeta (m).
 * @param up      radial local normalizada.
 * @param t       tiempo (s) — el MISMO reloj que alimenta `uDebug.y` en el shader.
 * @param depthM  profundidad del agua bajo el punto (m).
 * @param fade    0..1, igual que en el shader (fuera del clipmap la ola se apaga).
 */
inline float oceanWaveHeight(const glm::vec3& wp, const glm::vec3& up, float t,
                             float depthM, float fade = 1.0f) {
    const glm::vec3 t1 = glm::normalize(std::abs(up.y) < 0.99f ? glm::cross(up, glm::vec3(0, 1, 0))
                                                               : glm::cross(up, glm::vec3(1, 0, 0)));
    const glm::vec3 t2 = glm::cross(up, t1);
    float h = 0.0f;
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        const float lambda = OCEAN_WAVE[i][0];
        const float k      = 6.2831853f / lambda;
        const float amp    = oceanShoalAmp(depthM, OCEAN_WAVE[i][1]) * fade;
        if (amp <= 1e-4f) continue;
        const glm::vec3 D = glm::normalize(t1 * OCEAN_WAVE[i][2] + t2 * OCEAN_WAVE[i][3]);
        const float w  = std::sqrt(OCEAN_G * k);          // dispersión de aguas profundas
        const float ph = k * glm::dot(D, wp) - w * t;
        h += amp * std::sin(ph);                          // solo la componente a lo largo de `up`
    }
    return h;
}

}} // namespace Haruka::Planet
