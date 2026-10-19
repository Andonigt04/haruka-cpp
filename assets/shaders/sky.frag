/**
 * @file sky.frag
 * @brief El cielo POR FISICA: la luz del Sol y de la Luna dispersada por el aire (Rayleigh + Mie +
 *        ozono, `lib/atmosphere.glsl`) y las estrellas como fuentes puntuales por magnitud.
 *
 * Andoni (17-10): "la atmosfera los colores estan harcodeados no por atmosfera". Aqui habia una paleta
 * de cuatro colores, un naranja de atardecer fijo, un halo `pow(·, 8)` y un limbo aparte para verlo
 * desde fuera. Ahora es UNA integral por pixel: el azul, el horizonte blanco, el halo del Sol (Mie),
 * el atardecer, la sombra de la Tierra y la capa azul vista desde orbita salen de ella, desde el suelo
 * o desde el espacio, sin mezcla por altitud. La luz llega ya expuesta (`lib/sky_light.glsl`).
 *
 * Pase de fondo: se dibuja tras limpiar, SIN escribir profundidad; el terreno y los objetos encima. El
 * disco del Sol y la Luna son objetos de la escena (se SUMAN a esto, ver `ScenePass`).
 */
#version 450 core
#extension GL_GOOGLE_include_directive : require
#include "lib/sky_clouds.glsl"   // hash13 (estrellas) y el campo de nubes, para poder SONDEARLO
#include "lib/sky_light.glsl"    // la luz del frame (binding 31)
#include "lib/atmosphere.glsl"   // la integral

layout(location = 0) in vec3 vRayDir;
layout(location = 0) out vec4 FragColor;

// UBO compartido con sky.vert (binding 5) — antes uniforms sueltos (glUniform3fv/1f), que NO
// existen en Vulkan → van por UBO (ruta PSO/RHI). Declaración IDÉNTICA a la de sky.vert (GLSL lo
// exige entre etapas del mismo programa). Truco std140: vec3 (align 16, size 12) + float siguiente
// comparten el mismo slot de 16 B → por eso van emparejados.
layout(std140, binding = 5) uniform SkyParams {
    mat4  u_invViewProjRot; // inv(proj * mat4(mat3(view))) — lo usa el vertex
    vec3  u_sunDir;         // hacia el Sol (mundo, normalizado)
    float u_sunElev;        // dot(sunDir, up): elevación del sol
    vec3  u_up;             // cénit local del observador (mundo)
    float u_atmo;           // 1=superficie ... 0=espacio
    vec3  u_sunColor;       // color de la luz solar
    float u_time;           // segundos (movimiento de nubes)
    vec4  u_weather;        // x=humedad · y=tempC · z=precipitación · w=COBERTURA de nube (del mundo)
    vec4  u_wind;           // x=este(m/s) · y=norte(m/s) · z=racha · w=1 si es NIEVE
    vec4  u_planet;         // x=radio(m) · y=altitud del ojo(m) · z=base de nube(m)
};

// ── DÓNDE CORTA EL RAYO UNA CAPA DE NUBE A ALTITUD `h` ──────────────────────────────────────────
//
// Devuelve las coordenadas sobre la capa, en KILÓMETROS, y en `outOk` si el rayo la alcanza.
//
// ⚠️ Esto es lo que sustituye a `tangente / (t + 0.22)`, que era un plano SIN altitud: todas las
// capas se comprimían igual hacia el horizonte, así que dos capas a alturas distintas se movían
// idénticas y no había paralaje entre ellas. Con la intersección real, una capa baja barre deprisa
// sobre tu cabeza y un cirro apenas se mueve — que es lo que hace que el cielo tenga PROFUNDIDAD.
//
// Geometría: esfera de radio `R + h` centrada en el planeta, ojo a `R + camAlt` sobre el eje `up`.
// Se resuelve la cuadrática y se toma la raíz positiva (la capa está por encima del observador).
// Al ras del horizonte la distancia se dispara, así que se acota: más allá la nube es una franja
// borrosa y seguir estirando la coordenada solo produce aliasing.
vec2 cloudPlane(vec3 dir, vec3 e1, vec3 e2, float h, out bool outOk, out float outDistKm) {
    outOk = false; outDistKm = 0.0;
    float R = u_planet.x;
    if (R <= 0.0) {
        // Sin planeta (menús, tests): se conserva el domo de siempre para no dejar el cielo vacío.
        float t = max(dot(dir, u_up), 0.0);
        outOk = true; outDistKm = 4.0;
        return vec2(dot(dir, e1), dot(dir, e2)) / (t + 0.22) * 4.0;
    }
    float ro    = R + max(u_planet.y, 0.0);     // radio del ojo
    float rc    = R + h;                        // radio de la capa
    float mu    = dot(dir, u_up);               // coseno respecto al cénit
    float disc  = ro * ro * (mu * mu - 1.0) + rc * rc;
    if (disc < 0.0) return vec2(0.0);           // el rayo no llega a la capa
    float dist  = -ro * mu + sqrt(disc);        // raíz positiva: la capa está arriba
    if (dist <= 0.0) return vec2(0.0);
    // ⚠️⚠️ AQUI ESTABA EL ANILLO. Esto era `dist = min(dist, 6.0*h)`, un tope DURO, y el efecto no es
    // "la nube se ve borrosa a lo lejos" sino que **por debajo de cierta elevacion la capa desaparece
    // de golpe**. Con la capa a 4 km y el ojo a 300 m, alcanzar 6·h = 24 km pide asin(3700/24000) =
    // 8,9 grados; por debajo, TODOS los rayos se clavan en la misma distancia y —siendo casi
    // paralelos cerca del horizonte— muestrean el ruido practicamente en el MISMO punto. El campo
    // sale constante en toda esa franja y se va a todo-o-nada.
    //
    // Medido con `sky_layers_have_depth` antes de tocarlo: 0 % de nube por debajo de 10 grados,
    // 6,95 % y 12,73 % en 10-20. Un anillo de nube a altura fija de pantalla es exactamente lo que se
    // lee como "las nubes son un plano 2D".
    //
    // Se sustituye por una saturacion SUAVE: identica hasta `D` (el campo cercano no cambia ni un
    // bit) y a partir de ahi crece cada vez menos hasta `2·D`. Sigue acotada —que es lo que pedia el
    // aviso de aliasing— pero es MONOTONA y estrictamente creciente, asi que dos rayos vecinos nunca
    // caen en el mismo punto del campo y la capa sigue teniendo estructura hasta el horizonte.
    // La derivada vale 1 justo en `D`, asi que no hay codo visible en la transicion.
    {
        float D = 6.0 * max(h, 1.0);
        dist = (dist <= D) ? dist : D * (2.0 - D / dist);
    }
    vec3  hit   = dir * dist;                   // punto en la capa, relativo al ojo
    outOk = true; outDistKm = dist * 0.001;     // para el footprint del pixel (ver `fbmAA`)
    return vec2(dot(hit, e1), dot(hit, e2)) * 0.001;   // a km
}

void main()
{
    vec3 dir = normalize(vRayDir);
    const float R = uSkyPlanet.w;
    // Sin planeta (menus, pruebas): no hay aire; solo las estrellas.
    vec3 eye = (R > 0.0) ? -uSkyPlanet.xyz : vec3(0.0, 1.0e12, 0.0);

    vec3 T;
    vec3 L = (R > 0.0)
           ? harukaAtmoInscatter(eye, dir, -1.0, R, uSkySunDir.xyz, uSkySunE.rgb,
                                 uSkyMoonDir.xyz, uSkyMoonE.rgb, T)
           : vec3(0.0);
    if (R <= 0.0) T = vec3(1.0);

    // ── ESTRELLAS: fuentes PUNTUALES, detras de todo el aire ───────────────────────────────────
    // Irradiancia por magnitud visual (`uSkyMoonE.w` = la de magnitud 0, ya expuesta) repartida en
    // su celda: la radiancia es `π·E/Ω`. De dia no se ven porque la exposicion es ~1 y una estrella
    // es ~1e-11 del Sol, no porque nadie las apague; y el aire las atenua (y las tapa la Tierra).
    float g0, g1;
    const bool ground = (R > 0.0) && harukaAtmoRaySphere(eye, dir, R, g0, g1) && g0 > 0.0;
    if (!ground) {
        const float kCells = 350.0;                       // celdas por radian: Ω ≈ (1/350)²
        vec3  cell = floor(dir * kCells);
        float r    = hash13(cell);
        if (r > 0.9975) {
            // Magnitudes de -1 a 6,5 con muchas mas debiles que brillantes, como el cielo real.
            const float mag = mix(-1.0, 6.5, pow(hash13(cell + 3.1), 0.4));
            const float E   = uSkyMoonE.w * pow(10.0, -0.4 * mag);
            const float tw  = 0.75 + 0.25 * sin(hash13(cell + 7.0) * 100.0 + 0.0);   // titileo
            L += T * vec3(HARUKA_ATMO_PI * E * tw * kCells * kCells);
        }
    }
    FragColor = vec4(harukaDisplay(L), 1.0);
}
