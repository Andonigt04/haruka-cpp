/**
 * @file prop_vol.frag
 * @brief Fragment de MALLA INSTANCIADA QUE REPRESENTA UN VOLUMEN (`prop_vol.vert`): la silueta es la
 *        de la malla, la luz la del volumen que la envuelve (`lib/volume_light.glsl`). Mismo aire y
 *        misma salida que `prop_inst.frag`, para que se vea con el resto de la escena.
 *
 * El material llega por `u_material` (PropParams): x = opacidad · y = contraluz · z = dispersión
 * múltiple · w = rodeo de la luz al bulto. `u_flags.y` = ganancia del rim · `u_flags.z` = PINTURA (0 =
 * luz continua; 1 = acabado pintado de `lib/paint.glsl`: manchas de tono, trazos, borde descuidado) ·
 * `u_flags.w` = ancho del pincel en PÍXELES · `u_weather.x` = ángulo de un píxel (rad) · `u_weather.y` =
 * cuánto TAPA (< 1: translúcida a trazos, un velo) · `u_weather.z` = 1 si los trazos siguen el viento
 * (`u_wind`, unitario: las fibras del cirro).
 */
#version 450 core
#extension GL_GOOGLE_include_directive : require

#include "lib/frame_ubo.glsl"
#include "lib/volume_light.glsl"
#include "lib/aerial.glsl"
#include "lib/paint.glsl"

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;
layout(location = 2) in vec3 Color;
layout(location = 3) in vec2 TexCoord;
layout(location = 4) in vec3 AerialT;
layout(location = 5) in vec3 AerialS;
layout(location = 6) flat in vec4 VolC;
layout(location = 7) flat in vec2 VolR;

#include "lib/prop_params.glsl"

void main() {
    // El ojo está en el origen (pase cámara-relativo: ver prop_inst.frag).
    const vec3 V = normalize(-FragPos);
    const vec3 L = normalize(sunDirection);
    const vec3 N = normalize(Normal);

    const HarukaVolume vol = HarukaVolume(VolC.xyz, max(VolC.w, 1.0), max(VolR.x, 1.0), uSkyUp.xyz);
    // La forma de cada bulto pesa POCO: la nube es una masa, no un montón de globos.
    const HarukaVolumeParams prm = HarukaVolumeParams(u_material.x, u_material.z, u_material.w, u_material.y, 0.3);
    vec3 E = harukaVolumeLight(vol, FragPos, N, V, L, prm);

    if (u_flags.z > 0.0) {
        // EL PINCEL, en el marco del objeto (el punto respecto al centro de su envolvente): los trazos
        // van ALREDEDOR del cuerpo (horizontales, siguiendo la forma). El pincel es SIEMPRE EL MISMO
        // en pantalla, como el de un pintor: una nube grande lleva más trazos, no trazos más gordos,
        // y una lejana no se queda sin ellos. En metros se elige por OCTAVAS (potencias de 2) según
        // lo que mide un píxel ahí, fundiendo las dos vecinas: el trazo no baila al acercarse.
        const vec3 p = FragPos - VolC.xyz;
        const uint seed = uint(VolR.y);
        // El trazo va en UNA dirección horizontal por nube (girada por su semilla; la del viento si
        // `u_weather.z`), en un marco fijo de la nube (ver `harukaBrush`). ⚠️ Con "alrededor del eje"
        // (cross(vertical, normal)) los trazos eran círculos concéntricos, y en la panza plana, anillos.
        const vec3 ref = abs(vol.up.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        const vec3 h1 = normalize(cross(ref, vol.up)), h2 = cross(vol.up, h1);
        const float ang = float(seed & 1023u) * (6.2831853 / 1024.0);
        const vec3 along = (u_weather.z > 0.5 && dot(u_wind, u_wind) > 1e-6) ? u_wind : h1 * cos(ang) + h2 * sin(ang);
        // m por píxel: la distancia por el ángulo de un píxel. ⚠️ No `fwidth`: va a saltos de 2×2
        // píxeles y cambia de un triángulo a otro, y la octava del pincel salía en bloques.
        const float mpp = max(length(FragPos) * u_weather.x, 1e-3);
        const float lv = log2(max(u_flags.w, 1.0) * mpp);
        const float w0 = exp2(floor(lv)), fo = fract(lv);
        const uint so = seed + uint(int(floor(lv)) + 64);   // la semilla va con la octava (ver cloud_upsample.frag)
        const float brush = mix(harukaBrush(p, along, vol.up, w0, so), harukaBrush(p, along, vol.up, 2.0 * w0, so + 1u), fo);

        // VELO: con `u_weather.y` < 1 la capa es translúcida A TRAZOS (pincel seco): donde el trazo
        // carga poco no hay pintura y se ve el cielo. Sin mezcla ni orden: es un descarte.
        if (u_weather.y < 0.999) {
            const float dry = mix(harukaBrush(p, along, vol.up, 2.0 * w0, so + 101u),
                                  harukaBrush(p, along, vol.up, 4.0 * w0, so + 102u), fo);
            if (dry > u_weather.y) discard;
        }
        // BORDE DESCUIDADO: cerca de la SILUETA DE LA NUBE, donde el trazo es flojo, no hay pintura.
        // ⚠️ Con la normal de cada bolita del relieve, las bolitas vistas de canto DENTRO de la nube
        // también eran "borde" y la nube salía llena de huecos: el borde es el de la forma general
        // (la normal de la envolvente).
        const vec3 Nb = normalize(harukaVolumeScale(vol, p));
        if (harukaPaintRagged(1.0 - abs(dot(Nb, V)), brush, 0.3 * u_flags.z)) discard;

        // EL TRAZO EN LA LUZ: la luz continua entre su sombra más honda (el cielo que entra por la
        // panza) y su luz plena (Sol + cielo), movida por el trazo SOLO DONDE PINTA EL PINCEL (la
        // frontera luz/sombra y el contorno: `harukaPaintWhere`); el resto, liso. Las
        // sombras salen del color del cielo: azules. ⚠️ Se probó en 4 MANCHAS de tono (posterizado): en una panza la luz varía
        // suave y en círculo, y cada frontera entre tonos era un anillo — vetas de ágata.
        const vec3 Edark = harukaSkyAmbient(normalize(mix(vol.up, N, prm.shape)), vol.up) * 0.45;
        const vec3 Elit  = uSkySunGround.rgb + uSkyAmbientUp.rgb;
        const float lum  = dot(E - Edark, vec3(0.2126, 0.7152, 0.0722)) / max(dot(Elit - Edark, vec3(0.2126, 0.7152, 0.0722)), 1e-4);
        const float where = harukaPaintWhere(lum, 1.0 - abs(dot(Nb, V)));
        const float q = clamp(lum + (brush - 0.5) * 0.7 * where, 0.0, 1.0);
        E = mix(E, mix(Edark, Elit, q), u_flags.z);
    }
    vec3 color = harukaLit(Color, E);

    // RIM: la luz del cielo rozando la silueta de cada bulto, muy suave (el contorno pintado).
    if (u_flags.y > 0.0) {
        const float rim = pow(1.0 - max(dot(N, V), 0.0), 4.0) * u_flags.y;
        color += rim * 0.35 * harukaLit(vec3(1.0), uSkyAmbientUp.rgb + 0.3 * uSkySunGround.rgb);
    }

    color = harukaAerialApply(harukaOutputTonemap(color), AerialT, AerialS);
    FragColor = vec4(color, 1.0);
}
