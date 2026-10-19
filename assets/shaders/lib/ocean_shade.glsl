/**
 * @file ocean_shade.glsl
 * @brief Color del mar. Compartido por las DOS representaciones de la misma superficie.
 *
 * El océano se dibuja con dos geometrías, por la misma razón que el terreno:
 *   · CERCA — los anillos del clipmap, teselados, con olas de Gerstner de verdad (`ocean_wave.glsl`).
 *     Es donde una ola mide más de un píxel y por tanto donde tiene sentido moverla.
 *   · LEJOS — la esfera a nivel del mar, lisa. A esa distancia una ola de 60 m es subpíxel: darle
 *     geometría sería pagar vértices por un aliasing.
 * Las dos comparten ESTE sombreado, y las olas se desvanecen a 0 en el borde del clipmap, así que
 * donde se relevan describen exactamente la misma superficie.
 *
 * ⚠️ La orilla NO se decide aquí ni con un recorte contra la malla del terreno. La decide la
 * PROFUNDIDAD: la amplitud de la ola va a 0 con el fondo (ley de rompiente, ver `harukaShoalAmp`) y
 * el agua se descarta donde el fondo asoma. Ése es el arreglo del borde dentado de la costa — no hay
 * dos superficies que mantener de acuerdo, hay un campo de profundidad que se anula.
 */
#ifndef HARUKA_OCEAN_SHADE_GLSL
#define HARUKA_OCEAN_SHADE_GLSL

#include "lib/lowpoly.glsl"
#include "lib/sky_light.glsl"   // harukaLinear: los colores del agua son de pantalla

/**
 * @brief Luz que sale del mar hacia el ojo, en LINEAL (radiancia del motor, ya expuesta).
 *
 * ⚠️ ANTES ERA COLOR DE PANTALLA CON LUZ INVENTADA: quien llamaba pasaba `lightCol = vec3(1.0)` y un
 * ambiente fijo `(0,10, 0,13, 0,18)`, el cielo reflejado era ese ambiente por un tinte azul elegido, y
 * el brillo del Sol un `·2,2`. El mar brillaba igual de dia que de noche. Ahora entra la luz del frame
 * (`lib/sky_light.glsl`) y cada termino es fisico: el agua de debajo (difusa, con su color por
 * absorcion), el cielo REFLEJADO con Fresnel y el Sol reflejado con un lobulo que conserva la energia.
 * Quien llama le pone el aire de en medio y lo lleva a pantalla (`harukaDisplay`).
 *
 * @param wp       posición de la superficie del agua relativa al CENTRO del planeta (m).
 * @param fragPos  la misma posición relativa a la CÁMARA (m): de ahí salen vista y distancia.
 * @param n        normal de la superficie (ya con la ola, si la hay).
 * @param nF       normal de la LUZ DIRECTA (sol y brillo): la de la faceta en low-poly.
 * @param L        dirección al Sol (normalizada).
 * @param sunE     irradiancia del Sol que llega a la superficie (tras el aire), lineal y expuesta.
 * @param ambE     irradiancia difusa (cielo, suelo, Luna) sobre la superficie.
 * @param skyRad   radiancia del cielo en la dirección REFLEJADA (π·L).
 * @param depthM   profundidad del agua bajo este punto (m). Gobierna color y opacidad.
 * @param foam     0..1 de `harukaGerstner`: cuánto rompe la ola aquí.
 * @param facet    cuánto pesa la faceta (0..1): abre la banda difusa y el lóbulo especular.
 * @return rgb = radiancia lineal · a = opacidad sobre el fondo marino que hay detrás.
 */
vec4 harukaOceanShade(vec3 wp, vec3 fragPos, vec3 n, vec3 nF, vec3 L, vec3 sunE, vec3 ambE, vec3 skyRad,
                      float depthM, float foam, float facet) {
    vec3 V = normalize(-fragPos);
    vec3 H = normalize(L + V);
    // Low-poly: sol y brillo con la normal de FACETA; el Fresnel —que es un rim— con la suave, o cada
    // faceta al ras se encendería entera (ver lib/lowpoly.glsl).
    float ndl  = dot(nF, L);
    float diff = harukaFacetBand(max(ndl, 0.0), ndl, facet);
    // Fresnel del agua (F0 = 0,02: índice 1,33), Schlick.
    float F    = 0.02 + 0.98 * pow(1.0 - max(dot(n, V), 0.0), 5.0);
    // Lóbulo especular NORMALIZADO (Blinn-Phong, (k+8)/8): la energía del Sol reflejado no depende
    // de lo estrecho que sea. Estrecho (agua, no plástico) con la suave; facetado se abre a 48: un
    // lóbulo de 220 sobre una cara plana es todo-o-nada, la cara entera destella y se apaga.
    float k    = mix(220.0, 48.0, clamp(facet, 0.0, 1.0));
    float FH   = 0.02 + 0.98 * pow(1.0 - max(dot(H, V), 0.0), 5.0);
    // ANIME: el Sol reflejado son CHISPAS recortadas, no un brillo difuso. El lóbulo se corta con un
    // escalón corto (la chispa tiene borde) y su energía se reparte en lo que queda.
    const float lobe  = pow(max(dot(nF, H), 0.0), k);
    const float glint = smoothstep(0.35, 0.55, lobe);
    vec3  spec = sunE * ((k + 8.0) / 8.0) * mix(lobe, glint * 0.6, 0.75) * FH * max(ndl, 0.0);

    // Color propio del agua por absorción: el agua se come el rojo primero, por eso el mar somero
    // tira a turquesa y el profundo a azul marino. Los dos son colores de PANTALLA (como el resto de
    // materiales del motor): se pasan a lineal para iluminarlos.
    vec3  deep    = vec3(0.02, 0.11, 0.26);
    vec3  shallow = vec3(0.10, 0.42, 0.48);
    float shoal   = exp(-max(depthM, 0.0) * 0.12);          // 1 en la orilla → 0 a ~40 m
    vec3  body    = harukaLinear(mix(deep, shallow, shoal));

    // ANIME: el reflejo del cielo en un degradado LIMPIO (el Fresnel suavizado en tres tramos) y el
    // agua saturada: el color propio pesa más cerca, el cielo manda al ras.
    const float Fs = smoothstep(0.0, 1.0, F);
    vec3 col = body * 1.15 * (ambE + sunE * diff) * (1.0 - Fs) + Fs * skyRad + spec;

    // ESPUMA. Va al final y por MEZCLA, no por suma: la espuma no es un brillo, es aire mezclado con
    // agua — un material distinto, difuso y casi blanco, que TAPA el agua de debajo.
    // ANIME: la espuma son MANCHAS blancas con borde, no un velo: la cobertura se recorta.
    vec3 foamCol = harukaLinear(vec3(0.92, 0.95, 0.97)) * (ambE + sunE * diff);
    foam = smoothstep(0.30, 0.50, foam);
    col = mix(col, foamCol, foam);

    // OPACIDAD por camino óptico, no por profundidad vertical: al ras del agua la luz atraviesa
    // mucho más agua y el mar se cierra, mientras que mirando a plomo se ve el fondo. Es lo que hace
    // que la orilla sea transparente y el horizonte marino opaco.
    float pathM  = min(depthM / max(dot(V, normalize(wp)), 0.05), 600.0);
    float absorb = 1.0 - exp(-pathM * 0.055);
    // La espuma es opaca de por sí: una cresta rompiendo no deja ver el fondo aunque haya medio metro.
    return vec4(col, max(mix(0.06, 0.95, absorb), foam));
}

#endif // HARUKA_OCEAN_SHADE_GLSL
