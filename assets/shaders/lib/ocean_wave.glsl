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

/// Número de trenes de olas. Cuatro basta para romper la periodicidad a la vista; más es coste sin
/// lectura, porque las cortas caen bajo el píxel antes de aportar silueta.
const int HARUKA_WAVES = 4;

/// Longitud de onda (m), amplitud en AGUAS PROFUNDAS (m) y dirección de propagación (2D, en el
/// plano tangente local; se lleva a 3D con el marco del punto). Escala humana: la de 61 m es el
/// swell que da la silueta, la de 8,7 m el rizo que da el grano.
const vec4 HARUKA_WAVE[HARUKA_WAVES] = vec4[HARUKA_WAVES](
//     lambda   amp     dir.x    dir.y
    vec4(61.0,  0.85,   1.000,   0.000),
    vec4(37.0,  0.45,   0.766,   0.643),
    vec4(19.0,  0.22,   0.174,  -0.985),
    vec4( 8.7,  0.09,  -0.500,   0.866)
);

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
        float lambda = HARUKA_WAVE[i].x;
        float k      = 6.2831853 / lambda;
        float amp    = harukaShoalAmp(depthM, HARUKA_WAVE[i].y) * fade;
        if (amp <= 1e-4) continue;
        // Dirección en 3D: la 2D del tren, llevada al plano tangente del punto.
        vec3  D  = normalize(t1 * HARUKA_WAVE[i].z + t2 * HARUKA_WAVE[i].w);
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
    float shoreFoam = smoothstep(1.6, 0.7, depthM / max(HARUKA_WAVE[0].y * 2.0, 0.1));
    outFoam = clamp(max(foldFoam, shoreFoam) * fade, 0.0, 1.0);
    return disp;
}

#endif // HARUKA_OCEAN_WAVE_GLSL
