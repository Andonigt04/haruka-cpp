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

/// Ley de Green sola: cuánto crece la amplitud al perder fondo, SIN el límite de rompiente.
/// ⚠️ GEMELO de `oceanGreenGain` (core/planet/ocean_wave.h). Cualquier cambio va en los dos.
float harukaGreenGain(float depthM) {
    float d = max(depthM, 0.05);
    return pow(clamp(50.0 / d, 1.0, 40.0), 0.25);
}

/// Longitud de onda por debajo de la cual una ola deja de existir como GEOMETRÍA: el doble del
/// segmento de teselación de `ocean.tesc` (4 m), que es lo que pide Nyquist.
/// ⚠️ GEMELO de `OCEAN_MIN_QUAD_M`.
#define HARUKA_MIN_QUAD_M 4.0   // el segmento de teselacion mas fino de `ocean.tesc`

/**
 * @brief NÚMERO DE ONDA LOCAL en profundidad finita. ⚠️ GEMELO de `oceanWaveNumber`.
 *
 * ⚠️ AQUÍ ESTABA `sqrt(g·k)` —dispersión de AGUAS PROFUNDAS— usándose también en 30 cm de agua, y de
 * eso colgaban dos limitaciones. La relación correcta es `ω² = g·k·tanh(k·d)`: lo que se conserva al
 * entrar en el bajío es la FRECUENCIA, no la longitud de onda, así que `k` crece y `λ` se ACORTA
 * mientras la altura sube. Ésa es la razón física de que una ola rompa, y sin ella `H/λ ≈ 0,016` en
 * agua somera y el pliegue era imposible.
 *
 * Arranque explícito de Fenton-McKee (`k·d = x·[tanh(x^{3/4})]^{-2/3}`, con `x = k₀·d`), exacto en
 * los dos límites, más dos pasos de Newton.
 *
 * ⚠️ SALIDA RÁPIDA CON `x >= 10`, y no es una optimización: `1−tanh(10) = 2,1e-9` está por debajo del
 * eps del float, así que devolver `k₀` da EL MISMO BIT que el solver. El mar profundo no cambia.
 */
float harukaWaveNumber(float k0, float depthM) {
    float d = max(depthM, 0.02);
    float x = k0 * d;                             // = ω²d/g
    if (x >= 10.0) return k0;                     // aguas profundas: tanh(x) == 1 en float
    float t34 = tanh(pow(x, 0.75));
    float k = (x * pow(t34, -2.0 / 3.0)) / d;
    float w2 = HARUKA_G * k0;
    for (int it = 0; it < 2; ++it) {
        float th = tanh(k * d);
        float f  = HARUKA_G * k * th - w2;
        float df = HARUKA_G * th + HARUKA_G * k * d * (1.0 - th * th);
        if (abs(df) < 1e-12) break;
        k -= f / df;
        if (!(k > 0.0)) { k = k0; break; }
    }
    return k;
}

/**
 * @brief Desvanecido de las olas que se han hecho MÁS CORTAS QUE LA REJILLA.
 *        ⚠️ GEMELO de `oceanShortWaveFade`.
 *
 * Consecuencia directa de la dispersión finita: este TCS tesela a 4 m por segmento porque la ola más
 * corta medía 8,7 m, y con `λ` variable ese mismo tren mide 4 m en 30 cm de agua — Nyquist justo, y
 * por debajo más allá. Sin esto, muaré exactamente en la orilla.
 */
float harukaShortWaveFade(float k, float quadM) {
    // ⚠️ DE `quad` A `2·quad`, y estuvo una octava corrida: con λ = 2·quad la ola YA CABE (Nyquist) y
    // ahí tiene que valer 1. Estaba valiendo 0, y con el quad fino de 4 m eso dejaba el tren de 8,7 m
    // al 8,75 % a los pies del jugador — el mar se veía PLANO.
    float lo = max(quadM, HARUKA_MIN_QUAD_M);
    float hi = lo * 2.0;
    float lambda = 6.2831853 / max(k, 1e-6);
    return clamp((lambda - lo) / (hi - lo), 0.0, 1.0);
}

/// "El viento tiene aquí todo el recorrido que quiera": el océano.
/// ⚠️ GEMELO de `Haruka::Planet::WATER_FETCH_UNLIMITED`.
#define HARUKA_FETCH_UNLIMITED 1.0e7

/**
 * @brief Cuánta ola cabe con el FETCH que hay. ⚠️ GEMELO de `oceanFetchFactor`.
 *
 * ⚠️ ES LO QUE IMPIDE QUE UN LAGO DE MONTAÑA TENGA OLAS DE MAR ABIERTO. El oleaje sale de UN estado
 * global (el viento sobre el océano) y el bajío lo AMPLIFICA en agua somera: medido, un lago de
 * 3,21 m de fondo recibía 3,46 m de ola. Pierson-Moskowitz describe el mar plenamente desarrollado
 * (fetch infinito); JONSWAP el limitado por fetch: `Hs = 0,0016·U·sqrt(F/g)`.
 *
 * El viento se DEDUCE del estado (`Hs = 4·sqrt(Σa²/2)`, `U = sqrt(Hs·g/0,21)`) en vez de pasarlo:
 * así el factor no puede desincronizarse de la ola que corrige.
 */
float harukaFetchFactor(float fetchM) {
    if (fetchM <= 0.0 || fetchM >= HARUKA_FETCH_UNLIMITED * 0.5) return 1.0;
    float sum2 = 0.0;
    for (int i = 0; i < HARUKA_WAVES; ++i) { float a = harukaWaveAt(i).y; sum2 += a * a; }
    float Hs = 4.0 * sqrt(sum2 * 0.5);
    if (Hs <= 1e-4) return 1.0;
    float U   = sqrt(Hs * HARUKA_G / 0.21);
    float HsF = 0.0016 * U * sqrt(fetchM / HARUKA_G);
    return clamp(HsF / Hs, 0.0, 1.0);
}

/**
 * @brief Factor COMÚN que impone el límite de rompiente sobre la SUMA de los trenes.
 *        ⚠️ GEMELO de `oceanBreakScale` (core/planet/ocean_wave.h).
 *
 * ⚠️ ESTE ERA EL BUG, Y VIVIÓ AQUÍ MIENTRAS LA CPU YA ESTABA ARREGLADA. El tope `0.55·d` se aplicaba
 * a CADA tren por separado dentro de `harukaShoalAmp` y luego se sumaban los cuatro, así que la
 * altura total podía llegar a `4 × 0.55·d = 2,2·d`: más del doble de lo que hay de agua. El índice de
 * rompiente es una afirmación sobre la altura TOTAL de la ola, no sobre cada componente.
 *
 * Se veía en toda la costa, pero saltaba a la vista en los lagos de montaña, someros enteros: un lago
 * de 3,21 m de fondo recibía una ola de 5,93 m pico a pico — más alta que profundo era el lago.
 *
 * Se devuelve un factor común y no un tope por tren para que el REPARTO entre trenes no cambie: la
 * ola se encoge entera conservando su forma, en vez de recortarle la cresta a los trenes grandes.
 */
float harukaBreakScale(float depthM, float fetchM, float quadM) {
    float green = harukaGreenGain(depthM) * harukaFetchFactor(fetchM);
    float sum = 0.0;
    // ⚠️ CON EL DESVANECIDO DE ONDA CORTA DENTRO: el tope habla de la altura que de verdad hay.
    for (int i = 0; i < HARUKA_WAVES; ++i) {
        float k0 = 6.2831853 / harukaWaveAt(i).x;
        sum += harukaWaveAt(i).y * harukaShortWaveFade(harukaWaveNumber(k0, depthM), quadM);
    }
    float total = sum * green;
    float limit = 0.55 * max(depthM, 0.0);
    return (total > limit && total > 1e-6) ? (limit / total) : 1.0;
}

/// Cuánto está rompiendo la ola aquí: 0 en agua honda, →1 cuando el tope la aplasta.
/// ⚠️ GEMELO de `oceanBreakiness`.
float harukaBreakiness(float depthM, float fetchM, float quadM) {
    return clamp(1.0 - harukaBreakScale(depthM, fetchM, quadM), 0.0, 1.0);
}

/**
 * @brief ESCARPADO (Q) de un tren. ⚠️ GEMELO de `oceanSteepness`.
 *
 * El parámetro `breakiness` NO se usa, y eso es deliberado: se intentó abrir Q en la franja de
 * rompiente para que la cresta se enrollara (la voluta) y **se revirtió**. Como `Q = presupuesto /
 * (k·A·N)`, el desplazamiento horizontal `Q·A` no depende de la amplitud: en un lago de 30 cm de
 * fondo salían los mismos ~3 m que en mar abierto y la lámina se plegaba sobre sí misma. La voluta
 * de verdad pide shoaling de longitud de onda, que es otro trabajo. El parámetro se queda para que
 * la firma no cambie el día que se retome.
 */
float harukaSteepness(float k, float amp, float breakiness) {
    return min(0.75 / (k * amp * float(HARUKA_WAVES) + 1e-4), 1.0);
}

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
    return min(ampDeep * harukaGreenGain(depthM), 0.55 * depthM);
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
    float kDeep0 = 6.2831853 / W0.x;
    // La trepada sigue al tren que rompe: fase con el `k` LOCAL, frecuencia con la profunda.
    float k0 = harukaWaveNumber(kDeep0, max(depthRest, 0.05));
    float w0 = sqrt(HARUKA_G * kDeep0);
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
/**
 * ⚠️ `quadM` ES EL LADO DEL QUAD QUE VA A LLEVAR ESTA OLA, y sin el el mar aliaseaba desde 2 km.
 *
 * La teselacion se desvanece con la distancia (`ocean.tesc`: a 2 km el quad ya mide 8 m, a 9 km
 * 1 024 m) y la amplitud se desvanecia MUCHO mas lejos, por un radio fijo. Entre medias se dibujaba
 * ola a plena amplitud sobre una rejilla que no podia llevarla. Medido antes de esto:
 *
 *     2 000 m -> 1,09 muestras por onda corta  ·  amplitud 1,00
 *     5 000 m -> 0,54                          ·  amplitud 1,00
 *     9 000 m -> 0,01 (quad 1 024 m)           ·  amplitud 1,00
 *
 * Con el quad local, cada tren se apaga cuando su longitud baja de dos quads — Nyquist, el mismo
 * criterio que ya usa el desvanecido por profundidad. El alcance de la ola deja de ser un radio
 * elegido a mano y pasa a salir de lo que la geometria puede representar.
 */
vec3 harukaGerstner(vec3 wp, vec3 up, float t, float depthM, float fetchM, float quadM, float fade,
                    out vec3 outNormal, out float outFoam) {
    // Marco tangente local estable: se construye del propio `up`, no de un uniform, para que dos
    // puntos vecinos den marcos vecinos (y la normal no salte).
    vec3 t1 = normalize(abs(up.y) < 0.99 ? cross(up, vec3(0, 1, 0)) : cross(up, vec3(1, 0, 0)));
    vec3 t2 = cross(up, t1);

    // ⚠️ BAJÍO POR TREN, TOPE COMÚN. Igual que `oceanWaveHeight`/`oceanWaveVelocity` en el gemelo de
    // CPU: cada longitud de onda crece a su ritmo (Green), pero el límite de rompiente se aplica UNA
    // vez sobre la suma. Se calculan fuera del bucle porque no dependen del tren.
    float qm         = max(quadM, HARUKA_MIN_QUAD_M);
    float green      = harukaGreenGain(depthM) * harukaFetchFactor(fetchM);
    float scale      = harukaBreakScale(depthM, fetchM, qm);
    float breakiness = clamp(1.0 - scale, 0.0, 1.0);

    vec3  disp = vec3(0.0);
    // Derivadas del desplazamiento respecto a las dos tangentes: de ahí sale la normal SIN muestrear
    // puntos vecinos (analítica, igual que el terreno). `jac` acumula el pliegue de la superficie.
    vec3  dT1 = t1, dT2 = t2;
    float jac = 1.0;

    for (int i = 0; i < HARUKA_WAVES; ++i) {
        vec4  W      = harukaWaveAt(i);          // el tren vigente (subido por la CPU)
        // ⚠️ `W.x` ES LA LONGITUD DE AGUAS PROFUNDAS. La local sale de la dispersión finita; `w` —la
        // frecuencia, que es lo que se conserva— sigue saliendo de la profunda.
        float k0     = 6.2831853 / W.x;
        float k      = harukaWaveNumber(k0, depthM);
        float amp    = W.y * green * scale * fade * harukaShortWaveFade(k, qm);
        if (amp <= 1e-4) continue;
        // Dirección en 3D: la 2D del tren, llevada al plano tangente del punto.
        vec3  D  = normalize(t1 * W.z + t2 * W.w);
        float w  = sqrt(HARUKA_G * k0);           // la frecuencia NO cambia con el fondo
        float ph = k * dot(D, wp) - w * t;
        float c  = cos(ph), s = sin(ph);
        // Q = escarpado. 1 sería la cúspide exacta (la ola justo a punto de plegarse); se reparte
        // entre los trenes para que la suma no se pliegue sola en mar abierto.
        float Q  = harukaSteepness(k, amp, breakiness);

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
