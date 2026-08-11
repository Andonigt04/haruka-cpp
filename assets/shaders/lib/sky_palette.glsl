// ================================================================================================
// PALETA Y GRADIENTE BASE DEL CIELO — gemelo GLSL de `src/core/sky_ambient.{h,cpp}`.
//
// ── QUÉ ES ──────────────────────────────────────────────────────────────────────────────────────
// El cielo, reducido a lo que DOS consumidores necesitan compartir:
//   · `sky.frag`          → lo DIBUJA (y encima le pone resplandor, halo, nubes y estrellas).
//   · `sky_ambient` (CPU) → lo INTEGRA en armónicos esféricos para la luz AMBIENTE.
// Si los dos no parten del mismo cielo, las sombras dejan de corresponderse con lo que se ve.
//
// ── QUÉ CONTIENE, Y POR QUÉ SOLO ESTO ───────────────────────────────────────────────────────────
// Constantes y FUNCIONES PURAS: todo entra por parámetro. Nada aquí lee una entrada de vértice ni
// un uniform, porque el gemelo de CPU no tiene ni lo uno ni lo otro (mismo criterio que
// `lib/cube_face.glsl`). `dir`, `t` y `sunAmt` son estado POR PÍXEL y viven en `main()`, no aquí:
// un global de GLSL solo se puede inicializar con una expresión constante.
//
// ── DIVERGENCIAS DELIBERADAS (no son bugs, están decididas) ─────────────────────────────────────
// Solo el sol
//   · La CPU añade REBOTE DE SUELO bajo el horizonte. El shader ahí aplana al color de horizonte
//     porque el terreno lo tapa; para el ambiente eso es incorrecto (una cara que mira abajo debe
//     recoger el color del bioma, no azul de horizonte).
//   · La CPU atenúa por COBERTURA de nube con un escalar; el shader dibuja la forma de las nubes.
//     A la irradiancia difusa le da igual la forma: solo importa cuánta hay.
//   · El shader añade halo, disco y estrellas. No aportan irradiancia y NO deben entrar en el SH
//     (el sol ya se cuenta aparte como luz directa: meterlo aquí sería contarlo dos veces).
//
// ⚠️ CUALQUIER CAMBIO EN LA PALETA O EN EL GRADIENTE VA EN LOS DOS FICHEROS A LA VEZ.
// ================================================================================================
#ifndef HARUKA_SKY_PALETTE_GLSL
#define HARUKA_SKY_PALETTE_GLSL

// Los cuatro colores que definen el cielo. Todo lo demás se interpola entre ellos.
const vec3 HARUKA_ZENITH_NIGHT = vec3(0.012, 0.016, 0.045);
const vec3 HARUKA_HORIZ_NIGHT  = vec3(0.020, 0.024, 0.055);
const vec3 HARUKA_ZENITH_DAY   = vec3(0.18,  0.40,  0.82);
const vec3 HARUKA_HORIZ_DAY    = vec3(0.62,  0.74,  0.92);

/** @brief Factor día/noche. 0 = noche cerrada · 1 = día pleno.
 *  @param sunElev  dot(sunDir, up): elevación del sol sobre el horizonte local. */
float harukaSkyDay(float sunElev) {
    return smoothstep(-0.12, 0.22, sunElev);
}

/** @brief Color base del cielo en una dirección: gradiente horizonte→cénit, interpolado
 *  noche↔día. SIN sol, SIN nubes, SIN estrellas — solo la cúpula.
 *  @param t    dot(dir, up): 1 = cénit · 0 = horizonte · <0 bajo el horizonte.
 *  @param day  salida de harukaSkyDay().
 *
 *  Bajo el horizonte se aplana al color de horizonte. Para DIBUJAR está bien (lo tapa el
 *  terreno); el gemelo de CPU lo sustituye ahí por el rebote de suelo. */
vec3 harukaSkyBase(float t, float day) {
    vec3 zenithC = mix(HARUKA_ZENITH_NIGHT, HARUKA_ZENITH_DAY, day);
    vec3 horizC  = mix(HARUKA_HORIZ_NIGHT,  HARUKA_HORIZ_DAY,  day);

    float h = smoothstep(0.0, 0.55, max(t, 0.0));
    return mix(horizC, zenithC, h);
}

#endif // HARUKA_SKY_PALETTE_GLSL
