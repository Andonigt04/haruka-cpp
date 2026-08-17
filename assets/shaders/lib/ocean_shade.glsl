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

/**
 * @param wp       posición de la superficie del agua relativa al CENTRO del planeta (m).
 * @param fragPos  la misma posición relativa a la CÁMARA (m): de ahí salen vista y distancia.
 * @param n        normal de la superficie (ya con la ola, si la hay).
 * @param L        dirección a la luz (normalizada).
 * @param lightCol color de la luz directa.
 * @param ambient  ambiente del ciclo día/noche (uAmbient.xyz).
 * @param depthM   profundidad del agua bajo este punto (m). Gobierna color y opacidad.
 * @param foam     0..1 de `harukaGerstner`: cuánto rompe la ola aquí.
 * @return rgb = color iluminado · a = opacidad sobre el fondo marino que hay detrás.
 */
vec4 harukaOceanShade(vec3 wp, vec3 fragPos, vec3 n, vec3 L, vec3 lightCol, vec3 ambient,
                      float depthM, float foam) {
    vec3 V = normalize(-fragPos);
    vec3 H = normalize(L + V);
    float diff = max(dot(n, L), 0.0);
    float fres = pow(1.0 - max(dot(n, V), 0.0), 5.0);      // Schlick, exponente 5 (el clásico)
    float spec = pow(max(dot(n, H), 0.0), 220.0);          // brillo estrecho: agua, no plástico

    // Color propio del agua por absorción: el agua se come el rojo primero, por eso el mar somero
    // tira a turquesa y el profundo a azul marino. No son dos colores elegidos a ojo, es el mismo
    // color con distinto camino óptico.
    vec3  deep    = vec3(0.02, 0.11, 0.26);
    vec3  shallow = vec3(0.10, 0.42, 0.48);
    float shoal   = exp(-max(depthM, 0.0) * 0.12);          // 1 en la orilla → 0 a ~40 m
    vec3  body    = mix(deep, shallow, shoal);

    float sunD = clamp(diff * 1.6, 0.0, 1.0);
    vec3  sky   = ambient * vec3(0.9, 1.15, 1.6);           // el ambiente ya sigue el ciclo día/noche
    vec3  amb   = ambient + sky * (0.55 + 0.45 * fres);
    vec3  col   = body * (amb + lightCol * sunD * 1.1)
                + lightCol * spec * (0.05 + 0.95 * fres) * 2.2;

    // ESPUMA. Va al final y por MEZCLA, no por suma: la espuma no es un brillo, es aire mezclado con
    // agua — un material distinto, difuso y casi blanco, que TAPA el agua de debajo. Sumarla dejaba
    // láminas autoiluminadas que se veían igual a medianoche (el bug que ya tuvo `shallow_water`).
    vec3 foamCol = vec3(0.92, 0.95, 0.97) * (amb + lightCol * sunD);
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
