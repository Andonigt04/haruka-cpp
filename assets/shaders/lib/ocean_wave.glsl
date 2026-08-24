/**
 * @file ocean_wave.glsl
 * @brief OLAS DE GERSTNER con bajío y rompiente. La geometría del mar, no su color.
 *
 * ── POR QUÉ GERSTNER Y NO UN SENO ──────────────────────────────────────────────────────────────
 * Un seno da una ondulación simétrica: valles y crestas con la misma forma. El mar real no es eso —
 * las crestas son ESTRECHAS y agudas y los valles ANCHOS y planos. Gerstner lo produce porque cada
 * partícula describe un círculo: además de subir y bajar, se desplaza HORIZONTALMENTE hacia la
 * cresta. Ese apiñamiento es lo que afila la cresta, y es también lo que permite que la ola ROMPA:
 * cuando el desplazamiento horizontal supera cierto punto la superficie se pliega sobre sí misma.
 *
 * ── EL MARCO: ONDAS PLANAS EN 3D, NO EN UN PLANO TANGENTE ──────────────────────────────────────
 * ⚠️ La fase se calcula con `dot(K, wp)` sobre la posición MUNDIAL (relativa al centro del planeta),
 * NO sobre coordenadas del plano tangente del clipmap. Es deliberado y hay dos motivos:
 *   1. El clipmap se RE-ANCLA a saltos cuando el jugador camina. Con la fase atada a su marco, todo
 *      el tren de olas daría un salto en cada re-anclaje.
 *   2. Una parametrización tipo `wp.xz` (la que usaba el mar anterior) es degenerada fuera del
 *      ecuador: sus crestas se retuercen en espirales gigantes al acercarse a los polos.
 * Con `dot(K, wp)` las crestas son planos que cortan la esfera: continuas en todo el planeta, sin
 * costura ni polo. A escala de kilómetros la esfera es plana y el error no se ve; más allá, el
 * clipmap ya ha desvanecido la ola.
 *
 * ── DISPERSIÓN REAL ────────────────────────────────────────────────────────────────────────────
 * `ω = sqrt(g·k)` (aguas profundas). No es cosmética: fija que las olas LARGAS viajen más deprisa
 * que las cortas, que es lo que hace que un tren de oleaje se lea como oleaje y no como una textura
 * animada. Con una velocidad inventada, todas avanzan al unísono y el ojo lo detecta enseguida.
 */
#ifndef HARUKA_OCEAN_WAVE_GLSL
#define HARUKA_OCEAN_WAVE_GLSL

const float HARUKA_G = 9.81;

// El estado del mar (los cuatro trenes + la cota de la lámina) llega RESUELTO desde la CPU: aquí
// había una tabla `const` gemela a mano de la de `ocean_wave.h`, y deja de poder serlo en cuanto el
// oleaje depende del viento. Ver la nota larga de `ocean_params.glsl` — ahí está el porqué.
// ⚠️ RUTA DESDE LA RAÍZ DE SHADERS, NO HERMANA. El resolvedor de `#include` del backend busca desde
// `assets/shaders/`, no desde la carpeta del archivo que incluye. Puesto como `"ocean_params.glsl"`
// lo buscaba en `assets/shaders/ocean_params.glsl` y no lo encontraba: `ocean.tese` no compilaba y
// el pipeline del mar se quedaba SIN etapa de teselación, en silencio y sin que fallara ni un test.
#include "lib/ocean_params.glsl"

/**
 * @brief Bajío (shoaling): cuánto crece una ola al entrar en agua somera, y cuándo revienta.
 *
 * Ley de Green: la energía se conserva mientras el fondo sube, así que la amplitud va como
 * `d^(-1/4)`. Una ola de 0,85 m en 50 m de fondo llega a ~1,6 m con 3 m de fondo.
 *
 * Y el LÍMITE DE ROMPIENTE, que es lo que impide que la ola se convierta en un pico absurdo al
 * llegar a la orilla: una ola rompe cuando su altura supera ~0,78 veces la profundidad (índice de
 * rompiente clásico). Aquí la amplitud se acota a `0.55·d` — la mitad de esa altura, porque la
 * altura pico-valle es 2·amplitud. Pasado el límite la ola no crece: ROMPE, y lo que crece es la
 * espuma (ver `harukaBreakFoam`).
 *
 * ⚠️ Y ES LO QUE HACE QUE LA COSTA FUNCIONE: con `d → 0` la amplitud va a 0, así que la superficie
 * del agua se junta con el fondo EXACTAMENTE en la línea de costa. La orilla deja de ser una
 * frontera entre dos superficies que hay que mantener de acuerdo — es donde la profundidad se anula.
 *
 * @param depthM  profundidad del agua (m, positiva). 0 = orilla.
 * @param ampDeep amplitud en aguas profundas (m).
 */
float harukaShoalAmp(float depthM, float ampDeep) {
    float d = max(depthM, 0.05);
    float green = pow(clamp(50.0 / d, 1.0, 40.0), 0.25);   // ley de Green, acotada
    return min(ampDeep * green, 0.55 * depthM);            // límite de rompiente
}

/**
 * @brief SWASH: cuánto TREPA la lámina por la playa en este punto (m). Gemelo de `oceanRunup`.
 *
 * ⚠️ SIN ESTO LA ORILLA ESTÁ CONGELADA, y era lo más flojo de la costa. El bajío hace que la
 * amplitud vaya a 0 con el fondo (`harukaShoalAmp`), que es lo que da una línea de costa limpia —
 * pero también implica que la lámina NO SE MUEVE justo donde más se mira. La espuma de orilla era un
 * `smoothstep` sobre la profundidad: una banda fija, del mismo ancho a todas horas.
 *
 * Lo que falta no es más ola: es lo que pasa DESPUÉS de que rompa. La ola rota se convierte en un
 * frente (bore) que sube por la arena y vuelve a bajar. Eso es un movimiento de la LÁMINA, no del
 * oleaje, y por eso va aquí como un término aparte que se suma al nivel del agua.
 *
 * Se monta sobre la fase del tren DOMINANTE: la trepada sigue a la ola que rompe, no a un reloj
 * suelto. Y el peso decae mar adentro, así que en aguas abiertas esto vale exactamente 0 y el mar de
 * siempre no cambia.
 *
 * ⚠️ La amplitud se evalúa a `max(prof, 1 m)` y no a la profundidad local: `harukaShoalAmp` acota por
 * `0.55·d`, que con `d` NEGATIVA (o sea sobre la arena seca, que es justo donde la trepada tiene que
 * llegar) daría amplitud negativa y la lámina se hundiría en vez de subir.
 *
 * @param depthRest profundidad con la lámina EN REPOSO (m). Negativa sobre tierra.
 * @param wp        posición de la superficie en reposo, relativa al centro del planeta (m).
 * @param up        radial local.
 * @param t         tiempo (s).
 */
float harukaSwash(float depthRest, vec3 wp, vec3 up, float t) {
    vec4  W0 = harukaWaveAt(0);                       // el tren que da la silueta: el que rompe
    float k0 = 6.2831853 / W0.x;
    float w0 = sqrt(HARUKA_G * k0);
    vec3  t1 = normalize(abs(up.y) < 0.99 ? cross(up, vec3(0, 1, 0)) : cross(up, vec3(1, 0, 0)));
    vec3  t2 = cross(up, t1);
    vec3  D0 = normalize(t1 * W0.z + t2 * W0.w);

    // Altura de la ola AL ROMPER (ver la nota de arriba sobre el max).
    float aBreak = harukaShoalAmp(max(depthRest, 1.0), W0.y);
    // Alcance: la trepada solo existe en la franja somera. Más allá de unas pocas alturas de ola de
    // fondo esto se apaga y el mar abierto queda intacto.
    float reach  = max(aBreak * 6.0, 2.0);
    float wgt    = 1.0 - smoothstep(0.0, reach, max(depthRest, 0.0));
    // La trepada retrasa a la ola un cuarto de ciclo: el agua sube por la arena DESPUÉS de que la
    // cresta llegue, no a la vez. Sin el desfase la lámina y la cresta laten juntas y se lee como un
    // pulso, no como una ola que rompe y se derrama.
    float ph = k0 * dot(D0, wp) - w0 * t - 1.5707963;
    return aBreak * sin(ph) * wgt;
}

/**
 * @brief Desplazamiento de Gerstner en un punto, con su normal y su factor de rompiente.
 *
 * @param wp        posición de la superficie EN REPOSO, relativa al centro del planeta (m).
 * @param up        radial local normalizada (la vertical del sitio).
 * @param t         tiempo (s).
 * @param depthM    profundidad del agua bajo el punto (m). Gobierna bajío y rompiente.
 * @param fade      0..1: apaga las olas al alejarse (el borde del clipmap las lleva a 0, para que
 *                  el agua lejana coincida EXACTAMENTE con la esfera lisa que la dibuja).
 * @param outNormal normal de la superficie ondulada.
 * @param outFoam   0..1: cuánto está rompiendo aquí (cresta sobre-escarpada + límite de rompiente).
 * @return desplazamiento a sumar a `wp`.
 */
vec3 harukaGerstner(vec3 wp, vec3 up, float t, float depthM, float fade,
                    out vec3 outNormal, out float outFoam) {
    // Marco tangente local estable: se construye del propio `up`, no de un uniform, para que dos
    // puntos vecinos den marcos vecinos (y la normal no salte).
    vec3 t1 = normalize(abs(up.y) < 0.99 ? cross(up, vec3(0, 1, 0)) : cross(up, vec3(1, 0, 0)));
    vec3 t2 = cross(up, t1);

    vec3  disp = vec3(0.0);
    // Derivadas del desplazamiento respecto a las dos tangentes: de ahí sale la normal SIN muestrear
    // puntos vecinos (analítica, igual que el terreno). `jac` acumula el pliegue de la superficie.
    vec3  dT1 = t1, dT2 = t2;
    float jac = 1.0;

    for (int i = 0; i < HARUKA_WAVES; ++i) {
        vec4  W      = harukaWaveAt(i);          // el tren vigente (subido por la CPU)
        float lambda = W.x;
        float k      = 6.2831853 / lambda;
        float amp    = harukaShoalAmp(depthM, W.y) * fade;
        if (amp <= 1e-4) continue;
        // Dirección en 3D: la 2D del tren, llevada al plano tangente del punto.
        vec3  D  = normalize(t1 * W.z + t2 * W.w);
        float w  = sqrt(HARUKA_G * k);            // dispersión de aguas profundas
        float ph = k * dot(D, wp) - w * t;
        float c  = cos(ph), s = sin(ph);
        // Q = escarpado. 1 sería la cúspide exacta (la ola justo a punto de plegarse); se reparte
        // entre los trenes para que la suma no se pliegue sola en mar abierto.
        float Q  = 0.75 / (k * amp * float(HARUKA_WAVES) + 1e-4);
        Q = min(Q, 1.0);

        disp += D * (Q * amp * c) + up * (amp * s);
        // d(disp)/d(dirección de propagación) — lo que afila la cresta y, si pasa de 1, la pliega.
        float dq = Q * amp * k * s;
        float da = amp * k * c;
        dT1 += D * (-dq * dot(D, t1)) + up * (da * dot(D, t1));
        dT2 += D * (-dq * dot(D, t2)) + up * (da * dot(D, t2));
        jac -= Q * amp * k * s;                   // jacobiano: <0 = superficie plegada = rompiendo
    }

    outNormal = normalize(cross(dT1, dT2) * sign(dot(cross(dT1, dT2), up)));
    // ESPUMA DE ROMPIENTE, por dos vías que se refuerzan:
    //  · `jac` bajo: la cresta se ha sobre-escarpado y se pliega — es la definición geométrica de
    //    romper, y ocurra donde ocurra (mar abierto con viento o en la barra de la playa).
    //  · altura contra fondo: cerca de la orilla la ola alcanza su límite y revienta entera.
    float foldFoam  = smoothstep(0.55, 0.05, jac);
    float shoreFoam = smoothstep(1.6, 0.7, depthM / max(harukaWaveAt(0).y * 2.0, 0.1));
    outFoam = clamp(max(foldFoam, shoreFoam) * fade, 0.0, 1.0);
    return disp;
}

#endif // HARUKA_OCEAN_WAVE_GLSL
