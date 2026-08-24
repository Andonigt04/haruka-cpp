/**
 * @file ocean_params.glsl
 * @brief EL ESTADO DEL MAR, resuelto en la CPU y subido. Gemelo de `Haruka::Planet::OceanState`.
 *
 * ⚠️ POR QUÉ NO SE DERIVA AQUÍ. Antes los trenes de olas eran una tabla `const` en
 * `ocean_wave.glsl`, gemela a mano de la de `ocean_wave.h`: cuatro líneas de números que un humano
 * mantenía sincronizadas. Eso aguanta mientras el oleaje sea FIJO.
 *
 * En cuanto el mar depende del viento y de la marea, "el mismo gemelo" pasa a significar "las mismas
 * ENTRADAS y la misma fórmula". Y las entradas —el viento del sistema de clima, las posiciones de los
 * cuerpos de marea— no están en un vertex shader: habría que subirlas igualmente, y encima duplicar
 * la derivación. Subiendo la tabla YA RESUELTA hay un solo cálculo (`oceanStateFromWind`) y dos
 * lectores, que es la única forma de que no puedan divergir.
 *
 * Va en su propia lib, y no dentro de `ocean_wave.glsl`, porque lo necesitan también el FRAGMENT del
 * agua (para la cota de la lámina, que decide la orilla) y el mar LEJANO (para subir la esfera con la
 * marea) — y ninguno de los dos quiere arrastrar el código de Gerstner entero.
 */
#ifndef HARUKA_OCEAN_PARAMS_GLSL
#define HARUKA_OCEAN_PARAMS_GLSL

/// Número de trenes. Gemelo de `OCEAN_WAVES`.
const int HARUKA_WAVES = 4;

layout(std140, binding = 29) uniform OceanParams {
    /// (lambda m, amplitud en aguas profundas m, dir.x, dir.y) por tren, en el plano tangente local.
    vec4 uOceanWave[HARUKA_WAVES];
    /// x = cota de la lamina sobre el nivel del mar base (m), MAREA INCLUIDA.
    /// y = 1 si el bloque trae datos validos; 0 = usar el mar de referencia (ver abajo).
    vec4 uOceanMisc;
};

/// La tabla de REFERENCIA, la que estaba escrita a mano. Sigue aquí como red de seguridad: si el
/// bloque no se ató (pipeline nuevo, orden de bind mal), un UBO sin atar lee CEROS — y con lambda 0
/// el número de onda es infinito y el mar sale como ruido blanco. Con esto degrada a "el mar de
/// siempre", que es un fallo que se ve pero no destruye la escena.
vec4 harukaWaveAt(int i) {
    if (uOceanMisc.y > 0.5) return uOceanWave[i];
    if (i == 0) return vec4(61.0, 0.85,  1.000,  0.000);
    if (i == 1) return vec4(37.0, 0.45,  0.766,  0.643);
    if (i == 2) return vec4(19.0, 0.22,  0.174, -0.985);
    return             vec4( 8.7, 0.09, -0.500,  0.866);
}

/// Cota de la lámina del mar (m sobre el nivel base). 0 si el bloque no es válido.
float harukaSeaLevelM() { return (uOceanMisc.y > 0.5) ? uOceanMisc.x : 0.0; }

#endif // HARUKA_OCEAN_PARAMS_GLSL
