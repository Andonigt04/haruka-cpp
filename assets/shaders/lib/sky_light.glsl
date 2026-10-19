/**
 * @file sky_light.glsl
 * @brief LA LUZ DEL FRAME POR FISICA (binding 31): Sol y Luna con su irradiancia real, ya expuesta, y
 *        las dos funciones con las que TODO pase la lleva a pantalla.
 *
 * Gemelo de `SkyLightUBO` (Renderer/SkyLight.h). Lo llena la CPU con `World/Planet/Atmosphere.h` y
 * `SkyRadiometry.h`; aqui solo se lee.
 *
 * ── LA SALIDA A PANTALLA ─────────────────────────────────────────────────────────────────────────
 * La luz se calcula en LINEAL (irradiancia, radiancia). El motor escribe en un destino sin codificacion
 * sRGB y sus colores de material (terreno, props) estan pensados en espacio de PANTALLA. Las dos cosas
 * se casan con la gamma de la pantalla (2,2):
 *   · algo que EMITE o DISPERSA (cielo, disco del Sol): `harukaDisplay(radiancia)`;
 *   · una superficie con color de pantalla `c` bajo irradiancia `E`: la radiancia es `decode(c)·E`, y
 *     `encode(decode(c)·E) = c·E^(1/2,2)` EXACTO para una gamma potencia: `harukaLit(c, E)`.
 * Con `E = 1` (la luz de referencia) el color sale tal cual lo pinto quien lo eligio, y con menos luz
 * oscurece como oscurece la realidad, no como un multiplicador lineal sobre valores de pantalla.
 */
#ifndef HARUKA_SKY_LIGHT_GLSL
#define HARUKA_SKY_LIGHT_GLSL

layout(std140, binding = 31) uniform SkyLight {
    vec4 uSkySunDir;      // xyz hacia el Sol · w = radio angular del disco (rad)
    vec4 uSkySunE;        // rgb irradiancia solar en lo alto de la atmosfera, expuesta · w = exposicion
    vec4 uSkyMoonDir;     // xyz hacia la Luna · w = radio angular
    vec4 uSkyMoonE;       // rgb irradiancia de la Luna, expuesta · w = estrella de magnitud 0, expuesta
    vec4 uSkyPlanet;      // xyz centro del planeta MENOS el ojo (m) · w = radio (m)
    vec4 uSkySunGround;   // rgb el Sol que llega al ojo (tras el aire), expuesto
    vec4 uSkyMoonGround;  // rgb idem la Luna
    vec4 uSkyAmbientUp;   // rgb irradiancia del cielo sobre una cara que mira arriba
    vec4 uSkyAmbientDown; // rgb idem hacia abajo (lo que devuelve el suelo)
    vec4 uSkyUp;          // xyz vertical del ojo · w = altitud del ojo (m)
    vec4 uSkySunAtMoon;   // rgb el Sol en la Luna, por la transmitancia de la vista hasta ella, expuesto
    vec4 uSkyZenith;      // rgb radiancia del cielo al cenit (π·L), expuesta
    vec4 uSkyHorizon;     // rgb idem en el horizonte (media en acimut, a 3°)
};

const float HARUKA_DISPLAY_GAMMA = 2.2;

/**
 * @brief Radiancia lineal (ya expuesta) -> valor de pantalla.
 *
 * ⚠️ NO RECORTA POR CANAL. Con `clamp(x, 0, 1)` el horizonte del crepusculo (5,1, 2,2, 1,7 veces el
 * blanco: es lo que da la fisica con la camara abierta para la luz de 7 lux) salia BLANCO-ROSA — el
 * rojo topaba primero y el tono viraba. Aqui se comprime el canal MAYOR y los otros con el, asi que el
 * tono se conserva (naranja brillante), con una rodilla a 0,8 como la de las nubes. Solo cuando la
 * sobreexposicion es extrema (de 8 a 200 veces el blanco: el disco del Sol, no una nube al ocaso) se va a blanco,
 * que es lo que hace un sensor saturado.
 */
vec3 harukaDisplay(vec3 linear) {
    linear = max(linear, vec3(0.0));
    const float m = max(linear.r, max(linear.g, linear.b));
    const float knee = 0.8;
    if (m > knee) {
        const float mc = knee + (1.0 - knee) * (1.0 - exp(-(m - knee) / (1.0 - knee)));
        linear = mix(linear * (mc / m), vec3(mc), smoothstep(8.0, 200.0, m));
    }
    return pow(linear, vec3(1.0 / HARUKA_DISPLAY_GAMMA));
}
/// De pantalla a lineal (para componer luz sobre un color ya en pantalla).
vec3 harukaLinear(vec3 display) { return pow(max(display, 0.0), vec3(HARUKA_DISPLAY_GAMMA)); }
/// Superficie con color de PANTALLA `c` bajo irradiancia lineal `E` (ver la cabecera).
/// ⚠️ CON HOMBRO: un color claro bajo el Sol de cara (E ~ 1,5) pasaba de 1 y se clavaba en blanco
/// plano con halo de bloom (el hacha en la mano, gris 0,85, salía blanca entera: Andoni, 18-10). Como
/// en `harukaDisplay`, se comprime el canal MAYOR y los otros con él (el tono no vira) desde 0,8;
/// por debajo, `c·E^(1/2,2)` exacto.
/// El hombro de pantalla: desde 0,8 comprime el canal mayor (y los otros con él) hacia 1.
vec3 harukaShoulder(vec3 o) {
    const float m = max(o.r, max(o.g, o.b)), knee = 0.8;
    if (m > knee) o *= (knee + (1.0 - knee) * (1.0 - exp(-(m - knee) / (1.0 - knee)))) / m;
    return o;
}
vec3 harukaLit(vec3 c, vec3 E) {
    return harukaShoulder(c * pow(max(E, 0.0), vec3(1.0 / HARUKA_DISPLAY_GAMMA)));
}

/// Irradiancia DIRECTA (Sol + Luna, tras el aire) sobre una cara de normal `n`.
vec3 harukaSkyDirect(vec3 n) {
    return uSkySunGround.rgb * max(dot(n, uSkySunDir.xyz), 0.0)
         + uSkyMoonGround.rgb * max(dot(n, uSkyMoonDir.xyz), 0.0);
}
/// Irradiancia DIFUSA (cielo arriba, suelo abajo) sobre una cara de normal `n`, con vertical `up`.
vec3 harukaSkyAmbient(vec3 n, vec3 up) {
    const float t = 0.5 + 0.5 * dot(n, up);
    return mix(uSkyAmbientDown.rgb, uSkyAmbientUp.rgb, t);
}

/**
 * @brief Radiancia del cielo (π·L) en la direccion `d`, por su ELEVACION: del horizonte al cenit.
 * Lo que REFLEJA una superficie (el agua). Sin el acimut (el halo del Sol), que es luz directa y ya
 * entra por el brillo especular. Bajo el horizonte, el del horizonte (el suelo lejano lo tapa).
 */
vec3 harukaSkyRadiance(vec3 d, vec3 up) {
    const float e = max(dot(d, up), 0.0);
    return mix(uSkyHorizon.rgb, uSkyZenith.rgb, sqrt(e));
}

#endif // HARUKA_SKY_LIGHT_GLSL
