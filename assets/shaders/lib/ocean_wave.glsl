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
/**
 * @brief REFRACCION por el fondo. Gemelo EXACTO de `oceanRefract`.
 *
 * ⚠️ No existia: `D` era la direccion del tren y NO dependia del fondo, asi que las olas llegaban a
 * la playa con el angulo del mar abierto. En el mar real la ola se frena al perder fondo, gira, y
 * entra casi paralela a la orilla — por eso las crestas se ven paralelas a la playa.
 *
 * Snell para ondas de agua: el componente del vector de onda A LO LARGO de la isobata se conserva.
 * En agua profunda `k == k0` y devuelve `d0` exacto, asi que el mar abierto no cambia.
 *
 * `upSlope` = unitario en el plano tangente hacia agua MENOS profunda. El vector NULO = sin dato.
 */
vec3 harukaRefract(vec3 d0, vec3 upSlope, vec3 up, float k0, float k) {
    float slopeLen = length(upSlope);
    if (slopeLen < 1.0e-6 || k <= 0.0 || k0 <= 0.0) return d0;
    vec3  nS = upSlope / slopeLen;
    vec3  al = cross(up, nS);
    float alLen = length(al);
    if (alLen < 1.0e-6) return d0;
    vec3  aU = al / alLen;

    float a = dot(d0, nS);
    float b = dot(d0, aU);
    float kAlong = k0 * b;
    float k2 = k * k - kAlong * kAlong;
    // Reflexion total interna: la ola viaja por la isobata. Solo yendo hacia agua mas honda.
    if (k2 <= 0.0) return (b >= 0.0) ? aU : -aU;
    float kAcross = sqrt(k2) * ((a >= 0.0) ? 1.0 : -1.0);
    vec3  v = nS * kAcross + aU * kAlong;
    float vl = length(v);
    return (vl > 1.0e-9) ? (v / vl) : d0;
}

float harukaSteepness(float k, float amp, float breakiness) {
    return min(0.75 / (k * amp * float(HARUKA_WAVES) + 1e-4), 1.0);
}

/// Techo del escarpado TOTAL. Gemelo de `OCEAN_MAX_STEEP`. Es el mismo 0,75 que imponia
/// `harukaSteepness`, conservado a proposito: la formula nueva no puede quedar mas escarpada que la
/// vieja en ningun punto, asi que el pliegue del agua interior que obligo a revertir la voluta no
/// puede volver por aqui.
#define HARUKA_MAX_STEEP 0.75

/**
 * @brief Semieje HORIZONTAL de la orbita de Gerstner en fondo finito (m). Gemelo de `oceanHorizAmp`.
 *
 * ⚠️ El desplazamiento horizontal era `Q·A` con `Q = 0.75/(k·A·N)`, o sea `0.75/(k·N)`: **no dependia
 * de la amplitud**. Una charca de 30 cm recibia el mismo vaiven que el mar abierto. La orbita real es
 * una ELIPSE de semiejes `A/tanh(k·d)` (horizontal) y `A` (vertical): circulo en agua profunda,
 * aplastada y ancha en somero. Y escala con `A`, que es lo que faltaba.
 */
float harukaHorizAmp(float k, float amp, float depthM) {
    // Suelo del tanh: sin el, profundidad exactamente 0 en la orilla da NaN y se lleva el parche.
    float th = max(tanh(k * max(depthM, 0.0)), 1.0e-3);
    return amp / th;
}

/// Adelanto de la cresta al romper, como fraccion del semieje horizontal. Gemelo de `oceanCurlGain`.
///
/// ⚠️ Medido: con la orbita eliptica correcta el escarpado total llega como mucho a 0,818 y plegar
/// pide pasar de 1 — y quitando el tope entero se queda igual. No es el tope quien lo impide, es el
/// limite de rompiente. Lo que le falta a la ola no es escarpado, es ASIMETRIA: una ola que revienta
/// tiene la cara delantera casi vertical y la cresta adelantada respecto a su columna. Se anade como
/// un empujon del horizontal concentrado en `cos φ > 0`, proporcional a la amplitud — que es lo que
/// impide que vuelva el pliegue del agua interior del intento revertido.
#define HARUKA_CURL_MAX 1.10
#define HARUKA_BREAKER_MAX 1.60  // refuerzo direccional del tren que rompe. Gemelo de OCEAN_BREAKER_MAX
#define HARUKA_SKEW_MAX 0.25   // tope del 2º armonico: por encima al seno le sale un monticulo
// Banda de escarpado local (`1 - jacobiano`) en la que aparece la espuma de CRESTA. El punto medio
// (0,21) es el percentil que deja la cobertura de Monahan (1,56 % a 11,44 m/s) en mar abierto.
// ⚠️ Gemelos de `OCEAN_WHITECAP_LO` / `OCEAN_WHITECAP_HI`.
// Ajuste que liga la cola de la distribucion de pendientes con la cobertura empirica: si `|grad eta|`
// fuera gaussiano isotropo el umbral seria `sigma*sqrt(-2 ln W)`; los ocho trenes no cubren todas las
// direcciones, asi que la cola real es mas estrecha. Medido: percentil 98,44 = 0,186 con sigma 0,0981,
// o sea 1,897*sigma frente a 2,885 -> 0,657. ⚠️ Gemelos de OCEAN_WHITECAP_*.
#define HARUKA_WHITECAP_FIT 0.657
#define HARUKA_WHITECAP_LO  0.769
#define HARUKA_WHITECAP_HI  1.250

/// Indice de rompiente `H/d`: la altura que la ola QUIERE tener contra el fondo que hay. Gemelo de
/// `oceanBreakIndex`. Con la altura SIN recortar: el tope es quien la aplasta, asi que preguntarle a
/// la altura ya recortada si esta rompiendo seria circular.
float harukaBreakIndex(float depthM, float fetchM, float quadM) {
    if (!(depthM > 1.0e-4)) return 0.0;
    float green = harukaGreenGain(depthM) * harukaFetchFactor(fetchM);
    float sum = 0.0;
    for (int i = 0; i < HARUKA_WAVES; ++i) {
        vec4 W = harukaWaveAt(i);
        sum += W.y * harukaShortWaveFade(harukaWaveNumber(6.2831853 / W.x, depthM), quadM);
    }
    return 2.0 * sum * green / depthM;
}

float harukaCurlGain(float depthM, float fetchM, float quadM) {
    // ⚠️ Del indice de rompiente, no de `breakiness`: ese solo despega cuando el tope muerde
    // (`H/d = 1,1`) y romper empieza en 0,78, asi que la franja caia en 0,05-1,65 m cuando una ola de
    // 2,80 m rompe hacia 3,6 m. 0,78 es el indice clasico y 1,1 el tope del motor; se arranca en 0,60
    // porque la cara delantera se empina antes de plegarse.
    return HARUKA_CURL_MAX * smoothstep(0.60, 1.10, harukaBreakIndex(depthM, fetchM, quadM));
}

/// 2º ARMONICO de UN TREN, del Stokes de segundo orden. Gemelo de `oceanStokesSkew`.
///
/// ⚠️ Antes era una CONSTANTE (0,30) igual para los ocho trenes, con una puerta por profundidad. Eso
/// no es Stokes: la asimetria de una ola sale de SU escarpado (`k·a`) y de SU relacion con el fondo
/// (`k·d`), asi que un tren largo en 2 m se peralta mucho y uno corto casi nada.
///
///     a2 = (k·a²/4) · cosh(kd)·(2 + cosh 2kd) / sinh³(kd)      =>   S = a2/a
///
/// Limites: `kd -> inf` da el factor 2, o sea `S = k·a/2` (Stokes profundo); `kd -> 0` va como
/// `3/(kd)³`, el numero de Ursell — que es por lo que Stokes deja de valer en somero y hace falta el
/// tope. El tope 0,25 tampoco es un ajuste: con `S > 0,25` a `η = a(sin φ − S cos 2φ)` le salen dos
/// extremos de mas (en `sin φ = −1/(4S)`) y al seno le crece un monticulo secundario.
float harukaStokesSkew(float k, float amp, float depthM) {
    if (!(k > 0.0) || !(amp > 0.0)) return 0.0;
    float kd = k * max(depthM, 0.02);
    // Salida rapida obligatoria: por encima de kd ~ 10, cosh/sinh DESBORDAN y el cociente es inf/inf.
    float f = (kd >= 10.0) ? 2.0
                           : cosh(kd) * (2.0 + cosh(2.0 * kd)) / pow(sinh(kd), 3.0);
    return min(0.25 * k * amp * f, HARUKA_SKEW_MAX);
}

/// Refuerzo DIRECCIONAL de la rompiente. Gemelo de `oceanBreakerGain`.
///
/// ⚠️ ES LO QUE HACE QUE UNA OLA VUELQUE, y no por meter mas compresion: por CONCENTRARLA. El
/// escarpado por divergencia ya pasaba de 1, pero se repartia entre las dos direcciones del mapa
/// horizontal y un determinante no se anula asi — hace falta que UN autovalor llegue a 0. Medido en
/// una playa 1:20: a 5 m los autovalores eran 0,192 y 0,722. Reforzando solo el TREN 0 REFRACTADO
/// (crestas paralelas a la orilla, como un surf real) pasan a -0,127 y 0,948: pliega.
///
/// Arranca en el 0,78 clasico —el indice al que una ola revienta— y no en el 0,60 del adelanto de
/// cresta: peraltarse es una cosa y volcar es otra.
///
/// ⚠️ El agua interior esta a salvo por DOS puertas, y las dos se miden: el indice de rompiente no
/// llega a 0,78 en una charca, y el refuerzo es una FRACCION de `harukaHorizAmp`, que ya escala con la
/// amplitud. Una charca de 30 cm sale identica (horizontal max 0,879 m, determinante +0,609).
/// Fraccion de superficie con BORREGUILLOS en mar abierto (Monahan & O'Muircheartaigh 1980):
/// `W = 3,84e-6 * U^3,41`, con el viento DEDUCIDO del propio estado (`Hs = 4*sqrt(sum a^2/2)`,
/// `U = sqrt(Hs*g/0,21)`). Gemelo de `oceanWhitecapCoverage`. Es el oraculo del que sale el umbral de
/// espuma de cresta, en vez de elegirlo a mano.
float harukaWhitecapCoverage() {
    float sum2 = 0.0;
    for (int i = 0; i < HARUKA_WAVES; ++i) { float a = harukaWaveAt(i).y; sum2 += a * a; }
    float Hs = 4.0 * sqrt(sum2 * 0.5);
    if (Hs <= 1e-4) return 0.0;
    float U = sqrt(Hs * HARUKA_G / 0.21);
    return 3.84e-6 * pow(U, 3.41);
}

float harukaBreakerGain(float depthM, float fetchM, float quadM) {
    return HARUKA_BREAKER_MAX * smoothstep(0.78, 1.20, harukaBreakIndex(depthM, fetchM, quadM));
}

/// Factor COMUN que impide que la lamina se pliegue sobre si misma. Gemelo de `oceanSteepScale`.
/// Se encoge la ola ENTERA conservando su forma, igual que hace `harukaBreakScale` con la altura.
float harukaSteepScale(float depthM, float fetchM, float quadM) {
    float green = harukaGreenGain(depthM) * harukaFetchFactor(fetchM);
    float brk   = harukaBreakScale(depthM, fetchM, quadM);
    float sum   = 0.0;
    for (int i = 0; i < HARUKA_WAVES; ++i) {
        vec4  W   = harukaWaveAt(i);
        float k0  = 6.2831853 / W.x;
        float k   = harukaWaveNumber(k0, depthM);
        float amp = W.y * green * brk * harukaShortWaveFade(k, quadM);
        sum += k * harukaHorizAmp(k, amp, depthM);
    }
    // El techo SUBE donde la ola rompe: con 0,75 fijo la cresta no podia plegarse nunca. Fuera de esa
    // franja el techo es el de antes, asi que el mar abierto no cambia.
    float ceiling = HARUKA_MAX_STEEP
                  + (1.0 + HARUKA_CURL_MAX - HARUKA_MAX_STEEP)
                    * harukaCurlGain(depthM, fetchM, quadM) / max(HARUKA_CURL_MAX, 1e-6);
    return (sum > ceiling && sum > 1e-6) ? (ceiling / sum) : 1.0;
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
 * espuma (el `outFoam` de `harukaGerstner`, gemelo de `oceanFoam`).
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
                    vec3 upSlope, out vec3 outNormal, out float outFoam) {
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
    float steepScale = harukaSteepScale(depthM, fetchM, qm);
    float curl       = harukaCurlGain(depthM, fetchM, qm);
    float breaker    = harukaBreakerGain(depthM, fetchM, qm);

    vec3  disp = vec3(0.0);
    // Derivadas del desplazamiento respecto a las dos tangentes: de ahí sale la normal SIN muestrear
    // puntos vecinos (analítica, igual que el terreno). `jac` acumula el pliegue de la superficie.
    vec3  dT1 = t1, dT2 = t2;
    float jac = 1.0;
    // LA PENDIENTE de la superficie, acumulada en este mismo bucle (no cuesta otra pasada):
    // `grad eta = sum D*(amp*k*cos phi)`. ⚠️ NO es `1-jac`: ese es la divergencia del mapa horizontal
    // y lleva dentro el `1/tanh(k*d)` de la elipse orbital, que en el bajio se dispara sin que la ola
    // sea mas escarpada. Con el criterio equivocado la costa salia BLANCA. Gemelo del `outSlope` de
    // `oceanJacobian`.
    vec2 grad = vec2(0.0);
    float sumSq = 0.0;

    for (int i = 0; i < HARUKA_WAVES; ++i) {
        vec4  W      = harukaWaveAt(i);          // el tren vigente (subido por la CPU)
        // ⚠️ `W.x` ES LA LONGITUD DE AGUAS PROFUNDAS. La local sale de la dispersión finita; `w` —la
        // frecuencia, que es lo que se conserva— sigue saliendo de la profunda.
        float k0     = 6.2831853 / W.x;
        float k      = harukaWaveNumber(k0, depthM);
        float amp    = W.y * green * scale * fade * harukaShortWaveFade(k, qm);
        if (amp <= 1e-4) continue;
        // Dirección en 3D: la 2D del tren, llevada al plano tangente del punto.
        // ⚠️ REFRACTADA: la direccion de la tabla es la de aguas PROFUNDAS. Ver `harukaRefract`.
        vec3  D  = harukaRefract(normalize(t1 * W.z + t2 * W.w), upSlope, up, k0, k);
        float w  = sqrt(HARUKA_G * k0);           // la frecuencia NO cambia con el fondo
        float ph = k * dot(D, wp) - w * t;
        float c  = cos(ph), s = sin(ph);
        // EL SEMIEJE HORIZONTAL DE LA ELIPSE (ver `harukaHorizAmp`), no `Q·A`: el reparto viejo no
        // dependia de la amplitud, asi que una charca oscilaba de lado como el oceano.
        // El ADELANTO DE LA CRESTA (ver `harukaCurlGain`), solo en la mitad delantera.
        // ⚠️ EL REFUERZO VA SOLO AL TREN 0, Y ESE ES EL PUNTO: repartido entre los ocho comprime en
        // ocho direcciones y el determinante nunca llega a 0. Ver `harukaBreakerGain`.
        float horiz = harukaHorizAmp(k, amp, depthM) * steepScale * (1.0 + curl * max(0.0, c))
                    * ((i == 0) ? (1.0 + breaker) : 1.0);

        // El 2º armonico va en la VERTICAL: cresta picuda, seno plano. Ver `harukaStokesSkew`. No cambia
        // la altura pico-valle, asi que el limite de rompiente sigue significando lo mismo.
        float s2 = sin(2.0 * ph), c2 = cos(2.0 * ph);
        float skew = harukaStokesSkew(k, amp, depthM);      // POR TREN, no una constante comun
        disp += D * (horiz * c) + up * (amp * (s - skew * c2));
        // d(disp)/d(dirección de propagación) — lo que afila la cresta y, si pasa de 1, la pliega.
        // ⚠️ EL FACTOR DEL ADELANTO VA CON UN DOS, Y AQUI FALTABA. El desplazamiento es
        // `H·cos φ·(1 + g·max(0,cos φ))` = `H·(cos φ + g·cos²φ)` en la mitad delantera; su derivada
        // es `−H·sin φ·(1 + 2g·cos φ)`, no `−H·sin φ·(1 + g·cos φ)`. El termino del adelanto es
        // CUADRATICO en `cos φ`, asi que derivarlo dobla su coeficiente. Se dejaba la mitad del
        // efecto, y con ella la espuma de plegado apagada y la normal ligeramente mal.
        // Oraculo: la derivada numerica de `oceanDisplacement` (gemelo de CPU). Ver `ocean_break_fold`.
        float dHoriz = harukaHorizAmp(k, amp, depthM) * steepScale * (1.0 + 2.0 * curl * max(0.0, c))
                     * ((i == 0) ? (1.0 + breaker) : 1.0);
        float dq = dHoriz * k * s;
        // ⚠️ LA VERTICAL LLEVA EL 2º ARMONICO DERIVADO, y de aqui sale la NORMAL: la elevacion es
        // `amp·(sin φ − S·cos 2φ)`, cuya derivada es `amp·(cos φ + 2S·sin 2φ)`. Sin el segundo
        // termino la normal describiria una ola simetrica sobre una superficie que ya no lo es, y la
        // luz delataria la cresta picuda como un pliegue plano.
        float da = amp * k * (c + 2.0 * skew * s2);
        dT1 += D * (-dq * dot(D, t1)) + up * (da * dot(D, t1));
        dT2 += D * (-dq * dot(D, t2)) + up * (da * dot(D, t2));
        jac -= dq;                                // jacobiano: <0 = superficie plegada = rompiendo
        float ka = amp * k;
        grad += vec2(dot(D, t1), dot(D, t2)) * (ka * c);
        sumSq += ka * ka;                      // sigma de la pendiente: escala de la distribucion
    }

    outNormal = normalize(cross(dT1, dT2) * sign(dot(cross(dT1, dT2), up)));
    // ESPUMA DE ROMPIENTE, por dos vías que se refuerzan:
    //  · `jac` bajo: la cresta se ha sobre-escarpado y se pliega — es la definición geométrica de
    //    romper, y ocurra donde ocurra (mar abierto con viento o en la barra de la playa).
    //  · altura contra fondo: cerca de la orilla la ola alcanza su límite y revienta entera.
    // ⚠️ ESTA RAMA ERA `smoothstep(0.55, 0.05, jac)` Y ESTABA MUERTA: pedia un escarpado local de 0,45
    // y en mar abierto eso ocurre el 0,000 % de las veces (200 000 muestras). El oceano salia liso y
    // oscuro hasta la costa, cuando un mar de 11,4 m/s tiene crestas blancas.
    // El umbral nuevo esta CALIBRADO contra Monahan & O'Muircheartaigh (W = 3,84e-6·U^3,41): con el
    // viento que el estado deduce (11,44 m/s) toca 1,562 % y el percentil del escarpado ahi es 0,208.
    // Medido despues: 1,466 % en mar abierto, subiendo sola a 25 % en la rompiente.
    // ⚠️ EL UMBRAL NO SE ELIGE, SE DERIVA: se conoce la cobertura que toca (Monahan) y la escala de la
    // distribucion (sigma), asi que sale de invertir la cola. Con un umbral ABSOLUTO la cobertura se
    // disparaba x18,7 con x1,27 de viento; asi es Monahan por construccion. Y la blancura que crece
    // hacia la costa no sale de aqui sino de `shoreFoam`, que mira el indice de rompiente.
    float sg        = sqrt(sumSq * 0.5);
    float W         = clamp(harukaWhitecapCoverage(), 1.0e-5, 0.5);
    // ⚠️ SUELO OBLIGATORIO: con `fade = 0` todos los trenes salen por el `amp <= 1e-4`, `sg` queda en
    // CERO y `smoothstep(0,0,..)` da NaN — que no se ve como espuma rara, se lleva el pixel entero.
    float Tm        = max(sg * HARUKA_WHITECAP_FIT * sqrt(-2.0 * log(W)), 1.0e-6);
    float capFoam   = smoothstep(HARUKA_WHITECAP_LO * Tm, HARUKA_WHITECAP_HI * Tm, length(grad));
    // ⚠️ EL TERMINO QUE ORDENA EL PERFIL: la ola que ROMPE, no la profundidad. Con la banda por metros
    // que habia aqui, la espuma salia INVERTIDA (33 % a 8 m de fondo y 0 % a 1,5 m): el limite de
    // rompiente aplasta la amplitud justo donde la ola revienta, asi que la pendiente cae y un
    // criterio de pendiente no puede ver espuma donde la ola YA se rompio. El indice de rompiente si
    // lo ve, y crece monotono hacia la orilla. Arranca en el 0,78 clasico.
    float shoreFoam = smoothstep(0.78, 1.60, harukaBreakIndex(depthM, fetchM, qm));
    outFoam = clamp(max(capFoam, shoreFoam) * fade * harukaFoamScale(), 0.0, 1.0);
    return disp;
}

/// Sobrecarga SIN pendiente: la ola no refracta. La usan las sondas del banco y cualquier camino que
/// no tenga el gradiente del fondo a mano. `vec3(0)` es el centinela de "sin dato", el mismo que en
/// `oceanRefract`.
vec3 harukaGerstner(vec3 wp, vec3 up, float t, float depthM, float fetchM, float quadM, float fade,
                    out vec3 outNormal, out float outFoam) {
    return harukaGerstner(wp, up, t, depthM, fetchM, quadM, fade, vec3(0.0), outNormal, outFoam);
}

#endif // HARUKA_OCEAN_WAVE_GLSL
