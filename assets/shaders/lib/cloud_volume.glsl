/**
 * @file lib/cloud_volume.glsl
 * @brief LA NUBE COMO VOLUMEN: campo de densidad y perfil vertical, en un solo sitio.
 *
 * ── POR QUÉ EXISTE ──────────────────────────────────────────────────────────────────────────────
 * El cúmulo se pintaba dentro de `sky.frag`, en el pase de FONDO: sin profundidad, antes que la
 * escena, con un triángulo a pantalla completa. Eso lo convertía en un telón — cualquier objeto lo
 * tapaba aunque estuviera detrás, y sobre todo **no había un "dentro"**: era imposible atravesar
 * una nube porque un fondo no tiene interior. La única forma de fingir el interior habría sido un
 * efecto de pantalla aparte ("blanqueo"), que es un parche sobre el síntoma.
 *
 * La nube no puede ser una malla: si le pones superficie, desde dentro ves caras traseras y al
 * cruzarla hay un salto. Una nube es MEDIO PARTICIPATIVO — un campo de densidad que se recorre.
 * Con un raymarch, estar dentro deja de ser un caso especial: si el rayo empieza dentro, marcha
 * desde dentro, y la pérdida de visibilidad SALE SOLA en vez de programarse.
 *
 * ── UNA SOLA FUENTE ─────────────────────────────────────────────────────────────────────────────
 * ⚠️ El perfil vertical de aquí es el MISMO que `WeatherSystem::cloudDensityAt` en C++. Están
 * atados por `test_weather_3d` (`kProfileRise`/`kProfileFall`). Si divergen, el jugador entraría en
 * la niebla a una altura distinta de la que se ve la nube — que es exactamente el fallo que tenía
 * el clima cuando la CPU y el shader calculaban la nubosidad cada uno por su cuenta.
 */
#ifndef HARUKA_CLOUD_VOLUME_GLSL
#define HARUKA_CLOUD_VOLUME_GLSL

// Bordes de la losa, en fracción del grosor. GEMELOS de los de weather_system.cpp.
// La base es un plano bastante definido (el nivel de condensación es una cota, y por eso todas las
// nubes de un cielo tienen la panza a la misma altura); el techo se desfleca.
const float HARUKA_CLOUD_RISE = 0.28;
const float HARUKA_CLOUD_FALL = 0.62;

/** @brief Perfil vertical dentro de la losa. `t` = 0 en la base, 1 en el techo. */
float harukaCloudProfile(float t) {
    if (t <= 0.0 || t >= 1.0) return 0.0;
    float rise = smoothstep(0.0, HARUKA_CLOUD_RISE, t);
    float fall = 1.0 - smoothstep(HARUKA_CLOUD_FALL, 1.0, t);
    return rise * fall;
}

// ── RUIDO ───────────────────────────────────────────────────────────────────────────────────────
// Es el mismo value-noise + fBm que ya usaba el cielo. Vive aquí para que el fondo (cirro,
// altocúmulo) y el volumen (cúmulo) compartan la forma: si usaran ruidos distintos, la capa alta y
// la baja parecerían de dos cielos diferentes.
float harukaCloudHash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}
float harukaCloudVNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = harukaCloudHash13(vec3(i, 0.0));
    float b = harukaCloudHash13(vec3(i + vec2(1.0, 0.0), 0.0));
    float c = harukaCloudHash13(vec3(i + vec2(0.0, 1.0), 0.0));
    float d = harukaCloudHash13(vec3(i + vec2(1.0, 1.0), 0.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float harukaCloudFbm(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; ++i) { v += a * harukaCloudVNoise(p); p *= 2.0; a *= 0.5; }
    return v;
}

/** @brief Forma horizontal de la nube: domain warp (formas orgánicas, no manchas) + erosión de
 *  detalle en los bordes. `wind` desplaza el campo con el tiempo. */
float harukaCloudField(vec2 p, vec2 wind) {
    vec2  warp = vec2(harukaCloudFbm(p * 0.6 + wind * 0.5),
                      harukaCloudFbm(p * 0.6 + 5.2 - wind * 0.5));
    float d    = harukaCloudFbm(p * 1.3 + warp * 1.4 + wind);
    d -= 0.20 * harukaCloudFbm(p * 4.0 - wind * 2.0);
    return d;
}

/**
 * @brief Intersección de un rayo con un casquete esférico de radio `R`, con el origen del rayo en
 *        `ro` relativo al CENTRO de la esfera. Devuelve las dos raíces en `t0`/`t1`.
 * @return false si el rayo no corta.
 *
 * Se usa para entrar y salir de la losa: la nube es una cáscara entre R+base y R+techo, así que la
 * franja que hay que recorrer es la diferencia entre las dos esferas. Sobre un planeta esto NO es
 * lo mismo que dos planos horizontales: mirando al horizonte el rayo atraviesa muchísima más nube
 * que mirando al cénit, y eso es lo que hace que el cielo se cierre hacia el horizonte.
 */
bool harukaRaySphere(vec3 ro, vec3 rd, float R, out float t0, out float t1) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - R * R;
    float disc = b * b - c;
    if (disc < 0.0) { t0 = 0.0; t1 = 0.0; return false; }
    float s = sqrt(disc);
    t0 = -b - s;
    t1 = -b + s;
    return true;
}

#endif // HARUKA_CLOUD_VOLUME_GLSL
