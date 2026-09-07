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
const int HARUKA_WAVES = 8;

layout(std140, binding = 29) uniform OceanParams {
    /// (lambda m, amplitud en aguas profundas m, dir.x, dir.y) por tren, en el plano tangente local.
    vec4 uOceanWave[HARUKA_WAVES];
    /// x = cota de la lamina sobre el nivel del mar base (m), MAREA INCLUIDA.
    /// y = 1 si el bloque trae datos validos; 0 = usar el mar de referencia (ver abajo).
    /// z = EL RELOJ del oleaje (s). Vive aqui y no en el UBO del planeta porque es estado del MAR:
    ///     el pase de agua del quadtree no tiene aquel bloque, y sacarlo de otro sitio fue como el
    ///     agua acabo congelada una vez. Gemelo de `Haruka::Planet::oceanClockSeconds()`.
    /// w = ESCALA DE ESPUMA (`HARUKA_OCEAN_FOAM`, 1 por defecto). Existe para poder BISECAR en el
    ///     juego: apagar la espuma sin tocar la geometria de la ola es la unica forma de contestar
    ///     "¿lo blanco es la espuma?" mirando la pantalla.
    vec4 uOceanMisc;
};

/// La tabla de REFERENCIA, la que estaba escrita a mano. Sigue aquí como red de seguridad: si el
/// bloque no se ató (pipeline nuevo, orden de bind mal), un UBO sin atar lee CEROS — y con lambda 0
/// el número de onda es infinito y el mar sale como ruido blanco. Con esto degrada a "el mar de
/// siempre", que es un fallo que se ve pero no destruye la escena.
/// El reloj del oleaje (s). 0 si el bloque no se ato: el mar sale quieto, que es un fallo visible
/// pero no destruye la escena — el mismo criterio que la tabla de reserva de abajo.
float harukaOceanTime() { return (uOceanMisc.y > 0.5) ? uOceanMisc.z : 0.0; }

/// Escala de espuma para bisecar (`HARUKA_OCEAN_FOAM`). 1 si el bloque no se ato: sin dato, la espuma
/// se comporta como siempre — un 0 aqui apagaria la espuma en silencio, que es el fallo contrario.
float harukaFoamScale() { return (uOceanMisc.y > 0.5) ? uOceanMisc.w : 1.0; }

vec4 harukaWaveAt(int i) {
    // ⚠️ GUARDIA DE `lambda > 0`, y no es teorico: un UBO lleno A MEDIAS (un llamante que copie 4
    // trenes de 8, que es exactamente lo que paso al subir el espectro) pasa el `uOceanMisc.y > 0.5`
    // con los ultimos a cero, y `k = 2π/0` es infinito -> NaN -> **el agua no dibuja un solo pixel**.
    // Con esto, un tren sin rellenar simplemente no existe y el mar degrada en vez de desaparecer.
    if (uOceanMisc.y > 0.5) return (uOceanWave[i].x > 0.0) ? uOceanWave[i] : vec4(1.0, 0.0, 1.0, 0.0);
    // Red de seguridad: gemela EXACTA de `OCEAN_WAVE` (ocho trenes de Pierson-Moskowitz a 11,4 m/s,
    // reescalados para dar el mismo Hs = 2,8021 m que la tabla de cuatro que hubo). Si estas cifras y
    // las del .h se separaran, un UBO sin atar daria OTRO mar en vez del mismo.
    if (i == 0) return vec4(137.0, 0.5090,  0.98481, -0.17365);
    if (i == 1) return vec4( 89.0, 0.5390,  0.97030,  0.24192);
    if (i == 2) return vec4( 61.0, 0.4394,  1.00000,  0.00000);
    if (i == 3) return vec4( 43.0, 0.3492,  0.86603,  0.50000);
    if (i == 4) return vec4( 29.0, 0.2602,  0.89879, -0.43837);
    if (i == 5) return vec4( 19.0, 0.1748,  0.57358,  0.81915);
    if (i == 6) return vec4( 12.7, 0.1147,  0.30902, -0.95106);
    return             vec4(  8.7, 0.0742, -0.30902,  0.95106);
}

/// Cota de la lámina del mar (m sobre el nivel base). 0 si el bloque no es válido.
float harukaSeaLevelM() { return (uOceanMisc.y > 0.5) ? uOceanMisc.x : 0.0; }

#endif // HARUKA_OCEAN_PARAMS_GLSL
