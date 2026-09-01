/**
 * @file ocean_wave.h
 * @brief Gemelo CPU de `assets/shaders/lib/ocean_wave.glsl`. El oleaje para quien no dibuja.
 *
 * ⚠️ EXISTE PORQUE SIN ÉL EL AGUA TIENE DOS FORMAS. El shader levanta olas de metro y medio y la
 * física sigue viendo un plano: una barca flota atravesada, un nadador sube y baja por una ola que
 * no existe para él, y el ahogamiento se decide contra una cota que no es la que se ve. Es la misma
 * clase de discrepancia que costó una sesión entera con el terreno (lo que se pisa contra lo que se
 * dibuja), y se evita de la única forma que se evita: escribiendo las dos caras a la vez, con las
 * MISMAS cifras y el MISMO orden de operaciones.
 *
 * CUALQUIER CAMBIO EN UNA CONSTANTE VA EN LOS DOS FICHEROS.
 *
 * Qué NO hace: no evalúa espuma ni normal. La física solo necesita la COTA de la superficie; la
 * normal y la rompiente son cosa del sombreado y pagarlas aquí sería trabajo para nadie.
 */
#pragma once

#include <glm/glm.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include "core/planet/water_fill.h"   // WATER_FETCH_UNLIMITED: el fetch del oceano

namespace Haruka { namespace Planet {

inline constexpr float  OCEAN_G     = 9.81f;
inline constexpr int    OCEAN_WAVES = 4;

/**
 * @brief EL reloj del oleaje. Uno solo para todo el motor.
 *
 * ⚠️ EXISTE PORQUE HABÍA DOS. `planet.cpp` llenaba `uDebug.y` con un `static` suyo y
 * `planetary_system.cpp` evaluaba la ola de la física con OTRO `static`, cada uno inicializado en su
 * primera llamada — o sea en momentos distintos (el primer frame de render contra la primera consulta
 * de agua). El comentario de la física afirmaba ir "en fase con la que se ve" y no era verdad: el
 * desfase era la distancia entre esos dos instantes, que en una carga de mundo son segundos.
 *
 * Cuánto importa: la ola de 61 m tiene ω = √(g·k) = 1,005 rad/s. UN segundo de desfase es 1 rad, o
 * sea el 16 % del ciclo — **9,7 m de cresta**. Flotarías contra una ola que no es la dibujada, que es
 * exactamente la discrepancia que este fichero existe para impedir.
 *
 * Al ser `inline`, el estándar garantiza UNA sola instancia del `static` en todo el programa por más
 * unidades de traducción que lo incluyan. Ése es el punto: no se puede volver a duplicar por
 * descuido. `steady_clock` y no `high_resolution_clock` porque el segundo puede ser un alias de
 * `system_clock` y saltar con la hora del sistema — un salto de reloj teletransportaría el mar.
 */
inline float oceanClockSeconds() {
    static const auto s_start = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - s_start).count();
}

/// (longitud de onda m, amplitud en aguas profundas m, dir.x, dir.y). El mar de REFERENCIA: el que
/// había fijo en el código, y el que sigue saliendo cuando no hay viento que consultar.
///
/// No es una elección estética: reconstruyendo su altura significativa (`Hs = 4·σ`, con
/// `σ² = Σaᵢ²/2 = 0,491`) sale **Hs = 2,80 m**, que por Pierson-Moskowitz es el mar plenamente
/// desarrollado de un viento de **11,4 m/s**. O sea que la tabla que estaba escrita a mano ya era un
/// estado de mar concreto; `oceanStateFromWind` no la sustituye, la PARAMETRIZA.
inline constexpr float OCEAN_WAVE[OCEAN_WAVES][4] = {
    { 61.0f, 0.85f,  1.000f,  0.000f },
    { 37.0f, 0.45f,  0.766f,  0.643f },
    { 19.0f, 0.22f,  0.174f, -0.985f },
    {  8.7f, 0.09f, -0.500f,  0.866f },
};

/// Viento (m/s) al que la tabla de referencia corresponde. Ver la nota de `OCEAN_WAVE`.
inline constexpr float OCEAN_REF_WIND = 11.4f;

/**
 * @brief ESTADO DEL MAR: los cuatro trenes vigentes + la cota de la lámina. Lo que antes eran
 *        constantes de compilación.
 *
 * ⚠️ LO CALCULA LA CPU Y LA GPU SOLO LO LEE. Ésa es toda la arquitectura y es deliberada: el resto
 * del motor mantiene sus gemelos escribiendo la misma fórmula dos veces (aquí y en el .glsl), y eso
 * funciona mientras la fórmula sea FIJA. En cuanto el oleaje depende del viento, de la hora y del
 * clima, "la misma fórmula" pasa a ser "las mismas entradas Y la misma fórmula", y la primera mitad
 * no se puede garantizar desde un shader. Subiendo la tabla ya resuelta no hay nada que derivar dos
 * veces: hay UN cálculo y dos lectores.
 */
struct OceanState {
    /// (lambda m, amplitud m, dir.x, dir.y) por tren. Mismo formato y orden que `OCEAN_WAVE`.
    float wave[OCEAN_WAVES][4];
    /// Cota de la lámina en reposo sobre el nivel del mar base (m). Aquí entra la MAREA.
    float seaLevelM;
};

/// El estado por defecto: la tabla de referencia y el mar a su cota. Es lo que se ve si nadie
/// publica un estado — de modo que no configurar nada da exactamente el mar de antes.
inline OceanState oceanDefaultState() {
    OceanState s{};
    for (int i = 0; i < OCEAN_WAVES; ++i)
        for (int c = 0; c < 4; ++c) s.wave[i][c] = OCEAN_WAVE[i][c];
    s.seaLevelM = 0.0f;
    return s;
}

/**
 * @brief El mar que levanta un viento dado (Pierson-Moskowitz), girado a su rumbo.
 *
 * ── DE DÓNDE SALEN LAS CIFRAS ──────────────────────────────────────────────────────────────────
 * Mar plenamente desarrollado. Las dos relaciones clásicas de Pierson-Moskowitz:
 *   · Altura significativa   `Hs = 0,21·U²/g`
 *   · Frecuencia de pico     `ωp = 0,877·g/U`   →  con `ω²=gk`,  `λp = 2π·g/ωp² = 8,17·U²/g`
 * A 10 m/s dan Hs = 2,14 m y λp = 83 m, que son las cifras de manual. No son constantes inventadas:
 * es el motivo de que una galerna tenga olas largas y una brisa solo rizo.
 *
 * ── QUÉ SE CONSERVA DE LA TABLA DE REFERENCIA ──────────────────────────────────────────────────
 * Las PROPORCIONES entre los cuatro trenes (longitudes relativas, amplitudes relativas y los
 * ángulos entre ellos), porque son las que dan el aspecto ya afinado. El viento mueve la escala y el
 * rumbo; el reparto interno no se toca. A `U = OCEAN_REF_WIND` esto reproduce la tabla original.
 *
 * ⚠️ EL TREN CORTO NO SE ENCOGE. `ocean.tesc` tesela a 4 m por segmento porque Nyquist pide dos
 * muestras por longitud de onda y la ola más corta mide 8,7 m. Si el viento flojo redujera λ por
 * debajo de eso, esa ola dejaría de existir como geometría y aparecería como muaré. Se topa en 8,7 m:
 * físicamente también es lo razonable — el rizo de viento no escala con la mar de fondo.
 *
 * @param windSpeed     módulo del viento (m/s).
 * @param windDirX/Y    rumbo del viento en el plano tangente (no hace falta normalizarlo).
 * @param seaLevelM     cota de la lámina (marea incluida), en metros.
 */
inline OceanState oceanStateFromWind(float windSpeed, float windDirX, float windDirY,
                                     float seaLevelM = 0.0f) {
    OceanState s{};
    s.seaLevelM = seaLevelM;

    // Viento acotado. Por abajo: sin un mínimo el mar quedaría un espejo y la mar de fondo real no
    // desaparece aunque amaine. Por arriba: a 25 m/s Hs pasaría de 13 m y la ola dejaría de caber en
    // el clipmap (y arrasaría la costa) — un temporal así pide otro tratamiento, no más amplitud.
    const float U = std::clamp(windSpeed, 4.0f, 22.0f);

    // Escala de ALTURA: Hs va con U², así que la amplitud escala con (U/Uref)².
    const float ampScale = (U * U) / (OCEAN_REF_WIND * OCEAN_REF_WIND);
    // Escala de LONGITUD: λp también va con U². Misma razón, mismo factor.
    const float lamScale = ampScale;

    // Rumbo: se gira el marco de la tabla para que el tren principal siga al viento. Los otros tres
    // conservan su ángulo relativo, que es lo que rompe la periodicidad a la vista.
    const float wl = std::sqrt(windDirX * windDirX + windDirY * windDirY);
    const float cx = (wl > 1e-6f) ? windDirX / wl : 1.0f;
    const float cy = (wl > 1e-6f) ? windDirY / wl : 0.0f;

    for (int i = 0; i < OCEAN_WAVES; ++i) {
        // λ topada por abajo al tren corto de referencia (ver la nota de Nyquist arriba).
        s.wave[i][0] = std::max(OCEAN_WAVE[i][0] * lamScale, OCEAN_WAVE[OCEAN_WAVES - 1][0]);
        s.wave[i][1] = OCEAN_WAVE[i][1] * ampScale;
        // Rotación 2D del vector de dirección por el rumbo del viento.
        const float dx = OCEAN_WAVE[i][2], dy = OCEAN_WAVE[i][3];
        s.wave[i][2] = dx * cx - dy * cy;
        s.wave[i][3] = dx * cy + dy * cx;
    }
    return s;
}

/**
 * @brief MAREA: cota de la lámina (m) en una dirección, por el término de marea de Legendre P₂.
 *
 * `h = Σ GM·(3cos²θ − 1) / (2·D³) · escala`, que es el potencial de marea de un cuerpo puntual: dos
 * abultamientos opuestos (bajo la luna y en las antípodas), que es la razón de que haya DOS pleamares
 * al día y no una. `floorD` evita la singularidad si un cuerpo pasa por el centro.
 *
 * Gemelo del cálculo que ya vivía en `application_render.cpp` para el fluido — se trae aquí para que
 * la marea que ve la física, la que dibuja el mar y la que pinza los ríos sean UNA.
 */
inline double oceanTideAt(const glm::dvec3& dir, double planetRadius,
                          const glm::dvec3* bodyPos, const double* bodyGM, int bodyCount) {
    if (!bodyPos || !bodyGM || bodyCount <= 0 || planetRadius <= 0.0) return 0.0;
    const double scale  = (planetRadius * planetRadius / 9.8) * 15.0;
    const double floorD = planetRadius * 0.5;
    double h = 0.0;
    for (int i = 0; i < bodyCount; ++i) {
        const glm::dvec3 surf = dir * planetRadius;
        const glm::dvec3 to   = bodyPos[i] - surf;
        const double D = std::max(glm::length(to), floorD);
        const double L = std::max(glm::length(bodyPos[i]), 1.0);
        const double c = glm::dot(dir, bodyPos[i]) / L;
        h += bodyGM[i] * (3.0 * c * c - 1.0) / (2.0 * D * D * D);
    }
    return h * scale;
}

/// Ley de Green sola: cuánto crece la amplitud al perder fondo, SIN el límite de rompiente.
/// Gemelo de `harukaGreenGain`.
inline float oceanGreenGain(float depthM) {
    const float d = std::max(depthM, 0.05f);
    return std::pow(std::clamp(50.0f / d, 1.0f, 40.0f), 0.25f);
}

/// Longitud de onda por debajo de la cual una ola deja de existir como GEOMETRÍA: el doble del
/// paso de nodo más fino con el que el agua se dibuja (4 m), que es lo que pide Nyquist.
///
/// ⚠️ ES UN PISO, NO EL VALOR: el quad del render CRECE con la distancia (a 2 km ya mide 8 m, a 9 km
/// 1 024 m), así que el shader pasa el suyo y este número sólo actúa cerca del jugador. La CPU se
/// queda SIEMPRE en el piso, y es deliberado: la física es la referencia y actúa donde está el
/// jugador, que es justo donde el quad vale 4 m y los dos coinciden bit a bit.
/// ⚠️ APUNTABA A `ocean.tesc`, QUE YA NO EXISTE. El agua se dibujaba sobre la rejilla del clipmap y
/// esa era su teselación más fina; hoy la dibuja el pase de nodos (`terrain_node_water.vert`) y el
/// quad sale del propio nodo (`téxel × stride`). Este número es sólo el PISO de esa cadena.
inline constexpr float OCEAN_MIN_QUAD_M = 4.0f;

/**
 * @brief NÚMERO DE ONDA LOCAL en profundidad finita. Gemelo de `harukaWaveNumber`.
 *
 * ⚠️ ESTA ES LA PIEZA QUE FALTABA, Y DE ELLA COLGABAN DOS LIMITACIONES. Todo el oleaje usaba la
 * dispersión de AGUAS PROFUNDAS —`ω = √(g·k)`, escrito así y etiquetado así en los tres sitios—
 * también en 30 cm de agua. La relación correcta es `ω² = g·k·tanh(k·d)`, y la diferencia no es
 * cosmética: es la razón física de que una ola ROMPA.
 *
 * Lo que se conserva al entrar en el bajío es la FRECUENCIA (el periodo con el que llegan las
 * crestas), no la longitud de onda. Al perder fondo, `k` crece y `λ` se ACORTA mientras la altura
 * sube — y eso es lo que lleva la ola a la cúspide. Con `λ` fija, `H/λ ≈ 0,016` en agua somera y no
 * hay pliegue posible; forzarlo abriendo el escarpado fue lo que rompió el agua interior, porque
 * atacaba el síntoma.
 *
 * Medido con la tabla de referencia: el tren de 61 m pasa a **32,4 m en 3 m de fondo** y a ~10,7 m
 * en 30 cm. En mar abierto (40 m) queda en 60,7 m, o sea que **el mar profundo no cambia**.
 *
 * ── CÓMO SE RESUELVE, Y POR QUÉ ASÍ ─────────────────────────────────────────────────────────────
 *
 * `ω² = g·k·tanh(k·d)` es implícita en `k`. Se arranca con la forma explícita de Fenton-McKee,
 * `k·d = x·[tanh(x^{3/4})]^{-2/3}` con `x = ω²d/g`, que aquí vale EXACTAMENTE `k₀·d` (porque
 * `ω² = g·k₀`), y es exacta en los dos límites: `x` grande → `k = k₀` (profundo), `x` pequeño →
 * `k = √(k₀/d)` (somero). En medio se queda a ~1,7 %, y dos pasos de Newton lo cierran.
 *
 * ⚠️ SALIDA RÁPIDA EN AGUA PROFUNDA, Y NO ES UNA OPTIMIZACIÓN: es la GARANTÍA de que este cambio no
 * toca el mar abierto. Con `k₀·d ≥ 10`, `tanh` vale 1,0 EXACTO en float (1−tanh(10) = 2,1e-9, por
 * debajo del eps de 1,2e-7), así que devolver `k₀` no es una aproximación — es el mismo bit que
 * saldría del solver. El mar hondo queda idéntico al de antes, bit a bit.
 *
 * ⚠️ LIMITACIÓN ASUMIDA: `k` pasa a depender del punto, y la fase se sigue componiendo como `k·x`
 * en vez de integrar `k` a lo largo del rayo. Es la aproximación de número de onda LOCAL, estándar
 * en tiempo real, y se desvía donde la profundidad cambia rápido (un cantil submarino). Lo mismo
 * vale para la normal analítica del shader, que ignora el término `∂k/∂x`.
 */
inline float oceanWaveNumber(float k0, float depthM) {
    const float d = std::max(depthM, 0.02f);
    const float x = k0 * d;                       // = ω²d/g
    if (x >= 10.0f) return k0;                    // aguas profundas: tanh(x) == 1 en float
    // Fenton-McKee: arranque explícito, exacto en los dos límites.
    const float t34 = std::tanh(std::pow(x, 0.75f));
    float k = (x * std::pow(t34, -2.0f / 3.0f)) / d;
    // Dos pasos de Newton sobre `f(k) = g·k·tanh(k·d) − ω²`.
    const float w2 = OCEAN_G * k0;
    for (int it = 0; it < 2; ++it) {
        const float th = std::tanh(k * d);
        const float f  = OCEAN_G * k * th - w2;
        const float df = OCEAN_G * th + OCEAN_G * k * d * (1.0f - th * th);
        if (std::fabs(df) < 1e-12f) break;
        k -= f / df;
        if (!(k > 0.0f)) { k = k0; break; }        // sin NaN ni negativos: la física no los tiene
    }
    return k;
}

/**
 * @brief Desvanecido de las olas que se han hecho MÁS CORTAS QUE LA REJILLA. Gemelo de
 *        `harukaShortWaveFade`.
 *
 * ⚠️ ESTO ES CONSECUENCIA DIRECTA DE LA DISPERSIÓN FINITA Y HAY QUE PAGARLO. `ocean.tesc` tesela a
 * 4 m por segmento porque la ola más corta medía 8,7 m; con `λ` variable, ese mismo tren mide **4 m
 * en 30 cm de agua** — justo en Nyquist, y por debajo más allá. Sin este desvanecido, la ola más
 * corta se convertiría en muaré exactamente en la orilla, que es donde más se mira.
 *
 * Y es lo correcto físicamente además de necesario: una ola muy corta en agua muy somera disipa.
 *
 * ⚠️ VA EN LOS DOS LADOS aunque la CPU no tesele nada. La paridad no es negociable: si sólo el
 * shader desvaneciera, la ola que se nada dejaría de ser la que se ve — y `rhitest_waveprobe` lo
 * cazaría, que es justo para lo que está.
 */
inline float oceanShortWaveFade(float k, float quadM = OCEAN_MIN_QUAD_M) {
    // ⚠️ LA BANDA VA DE `quad` A `2·quad`, Y ESTUVO UNA OCTAVA CORRIDA. Nyquist dice que con λ = 2·quad
    // la ola YA CABE (dos muestras por longitud de onda), así que ahí tiene que valer 1 — y estaba
    // valiendo 0. Con el quad más fino (4 m) eso dejaba el tren de 8,7 m al **8,75 %** a los pies del
    // jugador: el mar se veía plano, y era este número. Por debajo de un quad no hay ni una muestra
    // por onda y ahí sí se apaga del todo.
    const float lo = std::max(quadM, OCEAN_MIN_QUAD_M);
    const float hi = lo * 2.0f;
    const float lambda = 6.2831853f / std::max(k, 1e-6f);
    return glm::clamp((lambda - lo) / (hi - lo), 0.0f, 1.0f);
}

/**
 * @brief Cuánta ola cabe con el FETCH que hay. Gemelo de `harukaFetchFactor`.
 *
 * ⚠️ ESTO ES LO QUE IMPIDE QUE UN LAGO DE MONTAÑA TENGA OLAS DE MAR ABIERTO. Medido antes de existir:
 * un lago de 3,21 m de fondo recibía **3,46 m de ola**, más alta que profundo era el lago, porque el
 * oleaje sale de UN estado global (el viento sobre el océano) y el bajío lo AMPLIFICA al entrar en
 * agua somera — el modelo no tenía forma de saber que ahí no hay recorrido para levantarla.
 *
 * El fetch es esa variable. Pierson-Moskowitz describe el mar PLENAMENTE DESARROLLADO (fetch
 * infinito) y es lo que `oceanStateFromWind` usa; JONSWAP describe el limitado por fetch:
 *
 *     Hs* = 0,0016 · sqrt(F*)      con  Hs* = g·Hs/U²  y  F* = g·F/U²   →   Hs = 0,0016·U·sqrt(F/g)
 *
 * A 11,4 m/s de viento y 400 m de lago sale **Hs = 0,12 m** contra los 2,80 m de mar abierto: un
 * factor de 24. Es la diferencia entre un lago y una bahía.
 *
 * ⚠️ EL VIENTO NO SE PASA: SE DEDUCE DEL PROPIO ESTADO. Añadirlo a `OceanState` sería un dato más que
 * mantener de acuerdo con los trenes, y ya se puede leer de ellos — `Hs = 4·sqrt(Σa²/2)` es la
 * definición de altura significativa, y `U = sqrt(Hs·g/0,21)` la invierte por PM. Así el factor no
 * puede desincronizarse de la ola que corrige.
 *
 * @param fetchM  diámetro equivalente de la masa de agua (m). `WATER_FETCH_UNLIMITED` = océano.
 */
inline float oceanFetchFactor(const OceanState& st, float fetchM) {
    // Océano, o sin dato: no se limita nada. `<= 0` es "este punto no trae fetch", no "fetch cero".
    if (fetchM <= 0.0f || fetchM >= WATER_FETCH_UNLIMITED * 0.5f) return 1.0f;
    float sum2 = 0.0f;
    for (int i = 0; i < OCEAN_WAVES; ++i) sum2 += st.wave[i][1] * st.wave[i][1];
    const float Hs = 4.0f * std::sqrt(sum2 * 0.5f);
    if (Hs <= 1e-4f) return 1.0f;
    const float U   = std::sqrt(Hs * OCEAN_G / 0.21f);          // el viento que da ese Hs (PM)
    const float HsF = 0.0016f * U * std::sqrt(fetchM / OCEAN_G); // JONSWAP limitado por fetch
    return glm::clamp(HsF / Hs, 0.0f, 1.0f);
}

/**
 * @brief Factor COMÚN que impone el límite de rompiente sobre la SUMA de los trenes. Gemelo de
 *        `harukaBreakScale`.
 *
 * ⚠️ ESTE ES EL ARREGLO. Antes el tope `0.55·d` se aplicaba a CADA tren por separado y luego se
 * sumaban los cuatro, así que la altura total podía llegar a `4 × 0.55·d = 2,2·d`: más del doble de
 * lo que hay de agua. El índice de rompiente es una afirmación sobre la altura TOTAL de la ola, no
 * sobre cada componente por separado, y tratarlo por componente lo vacía de sentido.
 *
 * Se veía en cualquier agua somera —toda la costa—, pero saltaba a la vista en los lagos de montaña,
 * que son someros enteros: medido, un lago de 3,21 m de fondo recibía una ola de 5,93 m pico a pico,
 * casi el doble que en mar abierto (3,16 m) y más alta que profundo era el lago.
 *
 * Se devuelve un factor común y no un tope por tren para que el REPARTO entre trenes no cambie: la
 * ola se encoge entera, conservando su forma, en vez de recortarle la cresta a los trenes grandes y
 * dejar intactos los pequeños (que daría un mar de otro aspecto al acercarse a la orilla).
 */
inline float oceanBreakScale(float depthM, const OceanState& st,
                             float fetchM = WATER_FETCH_UNLIMITED) {
    const float green = oceanGreenGain(depthM) * oceanFetchFactor(st, fetchM);
    float sum = 0.0f;
    // ⚠️ CON EL DESVANECIDO DE ONDA CORTA DENTRO. El tope habla de la altura que de verdad hay; si
    // sumara trenes que la dispersion finita ya ha apagado, acotaria una ola que no existe y dejaria
    // pasar mas de la que si.
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        const float k0 = 6.2831853f / st.wave[i][0];
        sum += st.wave[i][1] * oceanShortWaveFade(oceanWaveNumber(k0, depthM));
    }
    const float total = sum * green;
    const float limit = 0.55f * std::max(depthM, 0.0f);
    return (total > limit && total > 1e-6f) ? (limit / total) : 1.0f;
}

/**
 * @brief Cuánto está rompiendo la ola aquí: 0 en agua honda, →1 cuando el tope la aplasta.
 *        Gemelo de `harukaBreakiness`.
 *
 * Es exactamente "el límite de rompiente está mordiendo", que es la condición física de rompiente.
 * Lo consume el escarpado para plegar la cresta (la voluta) y la espuma.
 */
inline float oceanBreakiness(float depthM, const OceanState& st,
                             float fetchM = WATER_FETCH_UNLIMITED) {
    return glm::clamp(1.0f - oceanBreakScale(depthM, st, fetchM), 0.0f, 1.0f);
}

/**
 * @brief ESCARPADO (Q) de un tren. Gemelo de `harukaSteepness`.
 *
 * Q reparte el desplazamiento entre horizontal y vertical: `disp = D·(Q·A·cos φ) + up·(A·sin φ)`.
 * La superficie se PLIEGA —la cresta se echa hacia delante y se enrolla, que es lo que hace una ola
 * al romper— cuando `Σ Q·A·k > 1`, porque ahí el jacobiano se vuelve negativo.
 *
 * ⚠️ Con la fórmula anterior eso NO PODÍA PASAR NUNCA. Era `Q = 0.75/(k·A·N)` acotado a 1, lo que
 * deja `Q·A·k ≤ 0.75/N` por tren y por tanto `Σ ≤ 0.75`: el jacobiano se quedaba en 0,25 como
 * mínimo. El comentario de al lado decía "si pasa de 1, la pliega" describiendo un caso que la
 * propia fórmula impedía, y la espuma por plegado no se encendía jamás.
 *

 * ⚠️ SE INTENTO LA VOLUTA (la cresta que se enrolla al romper) Y SE REVIRTIO. Se subia el presupuesto
 * de escarpado y se abria el tope de Q en la franja de rompiente, y el jacobiano SI se volvia negativo
 * (medido: -0,104 a 1,2 m de fondo). Pero rompia el agua interior, y el motivo esta en la propia
 * formula: como `Q = presupuesto/(k·A·N)`, el desplazamiento horizontal `Q·A = presupuesto/(k·N)` NO
 * DEPENDE DE LA AMPLITUD. En mar abierto son ~3 m y esta bien; en un lago de 30 cm de fondo son los
 * MISMOS ~3 m, asi que la lamina se desplazaba metros en horizontal y se plegaba sobre si misma. Sobre
 * las celdas de 4,4 m del parche de rios/lagos eso se veia como cubos deformados.
 *
 * Y el fondo del asunto es que aqui la ola NO PUEDE plegarse sola: una ola real rompe porque al perder
 * fondo se le acorta la LONGITUD DE ONDA a la vez que crece la altura, y aqui lambda es fija. Con
 * H/lambda ~ 0,016 en agua somera no hay pliegue posible sin forzarlo, y forzarlo es lo que rompio el
 * agua interior. La voluta de verdad pide shoaling de longitud de onda, que es otro trabajo.
 */
inline float oceanSteepness(float k, float amp, float /*breakiness*/) {
    // Reparto del escarpado entre los trenes para que la suma no se pliegue sola en mar abierto.
    return std::min(0.75f / (k * amp * float(OCEAN_WAVES) + 1e-4f), 1.0f);
}

/// Ley de Green + límite de rompiente POR TREN. Se mantiene para el `swash`, que la usa como
/// medida de alcance de UN tren (el dominante) y no como amplitud de la ola sumada.
inline float oceanShoalAmp(float depthM, float ampDeep) {
    return std::min(ampDeep * oceanGreenGain(depthM), 0.55f * depthM);
}

/**
 * @brief SWASH: cuánto TREPA la lámina por la playa (m). Gemelo EXACTO de `harukaSwash`.
 *
 * ⚠️ ESTE GEMELO NO ES OPCIONAL. El swash mueve la LÁMINA, así que mueve la línea de costa: si solo
 * existiera en el shader, la orilla que se ve y la que se nada volverían a ser dos — que es la
 * discrepancia que este fichero entero existe para impedir, reintroducida por la puerta de atrás.
 *
 * El fenómeno: la ola rompe y se convierte en un frente que sube por la arena y vuelve a bajar. No es
 * oleaje (el bajío ya lleva la amplitud a 0 en la orilla, y por eso la costa sale limpia); es un
 * término aparte que se suma al nivel del agua y solo vive en la franja somera.
 *
 * @param depthRest profundidad con la lámina EN REPOSO (m). Negativa sobre tierra.
 */
inline float oceanSwash(float depthRest, const glm::vec3& wp, const glm::vec3& up, float t,
                        const OceanState& st) {
    const float lambda0 = st.wave[0][0];
    const float kDeep0  = 6.2831853f / lambda0;
    // ⚠️ La trepada sigue al tren que rompe, así que su fase va con el `k` LOCAL — el mismo que usa
    // la geometría. La frecuencia, en cambio, es la profunda: es lo que se conserva.
    const float k0 = oceanWaveNumber(kDeep0, std::max(depthRest, 0.05f));
    const float w0 = std::sqrt(OCEAN_G * kDeep0);
    const glm::vec3 t1 = glm::normalize(std::abs(up.y) < 0.99f ? glm::cross(up, glm::vec3(0, 1, 0))
                                                               : glm::cross(up, glm::vec3(1, 0, 0)));
    const glm::vec3 t2 = glm::cross(up, t1);
    const glm::vec3 D0 = glm::normalize(t1 * st.wave[0][2] + t2 * st.wave[0][3]);

    // Amplitud a `max(prof, 1 m)`: con profundidad negativa (arena seca, que es a donde la trepada
    // tiene que llegar) el tope `0.55·d` de `oceanShoalAmp` daría amplitud NEGATIVA.
    const float aBreak = oceanShoalAmp(std::max(depthRest, 1.0f), st.wave[0][1]);
    const float reach  = std::max(aBreak * 6.0f, 2.0f);
    const float wgt    = 1.0f - glm::smoothstep(0.0f, reach, std::max(depthRest, 0.0f));
    // Cuarto de ciclo de retraso respecto a la cresta: el agua sube DESPUÉS de que rompa.
    const float ph = k0 * glm::dot(D0, wp) - w0 * t - 1.5707963f;
    return aBreak * std::sin(ph) * wgt;
}

/**
 * @brief Cota de la superficie del agua sobre el reposo, en metros. Gemelo de `harukaGerstner`,
 *        pero devolviendo SOLO la componente vertical (a lo largo de `up`).
 *
 * ⚠️ El desplazamiento de Gerstner también es HORIZONTAL —es lo que afila la cresta—, así que la
 * superficie no es una función de la posición: un punto del plano puede estar bajo dos partes de la
 * ola. Para flotación y ahogamiento eso no importa (basta la altura en la vertical del punto) y
 * resolverlo bien exigiría invertir el desplazamiento, que es iterativo y caro. Se documenta el
 * límite en vez de esconderlo: con mar gruesa y una cresta muy inclinada, la cota que devuelve esto
 * puede quedarse hasta un `Q·amplitud` corta respecto a lo que se ve.
 *
 * @param wp      posición de la superficie en reposo, relativa al CENTRO del planeta (m).
 * @param up      radial local normalizada.
 * @param t       tiempo (s) — el MISMO reloj que alimenta `uDebug.y` en el shader.
 * @param depthM  profundidad del agua bajo el punto (m).
 * @param fade    0..1, igual que en el shader (fuera del clipmap la ola se apaga).
 * @param st      estado del mar (los cuatro trenes). El MISMO que se subió a la GPU este frame.
 */
inline float oceanWaveHeight(const glm::vec3& wp, const glm::vec3& up, float t,
                             float depthM, float fade, const OceanState& st,
                             float fetchM = WATER_FETCH_UNLIMITED) {
    const glm::vec3 t1 = glm::normalize(std::abs(up.y) < 0.99f ? glm::cross(up, glm::vec3(0, 1, 0))
                                                               : glm::cross(up, glm::vec3(1, 0, 0)));
    const glm::vec3 t2 = glm::cross(up, t1);
    // Bajío y tope de rompiente: el bajío es por tren (cada longitud de onda crece a su ritmo) pero
    // el tope es COMÚN a los cuatro, porque el índice de rompiente habla de la ola entera.
    // El FETCH entra multiplicando el bajío: es cuánta ola puede haber aquí antes de que el fondo la
    // amplifique. Sin él, un lago hereda el swell del océano — ver `oceanFetchFactor`.
    const float green = oceanGreenGain(depthM) * oceanFetchFactor(st, fetchM);
    const float scale = oceanBreakScale(depthM, st, fetchM);
    float h = 0.0f;
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        // ⚠️ `λ` DE LA TABLA ES LA DE AGUAS PROFUNDAS. La local sale de la dispersión finita, y `ω`
        // —que es lo que se conserva— sigue siendo la profunda. Ver `oceanWaveNumber`.
        const float k0 = 6.2831853f / st.wave[i][0];
        const float k  = oceanWaveNumber(k0, depthM);
        const float amp = st.wave[i][1] * green * scale * fade * oceanShortWaveFade(k);
        if (amp <= 1e-4f) continue;
        const glm::vec3 D = glm::normalize(t1 * st.wave[i][2] + t2 * st.wave[i][3]);
        const float w  = std::sqrt(OCEAN_G * k0);         // la frecuencia NO cambia con el fondo
        const float ph = k * glm::dot(D, wp) - w * t;
        h += amp * std::sin(ph);                          // solo la componente a lo largo de `up`
    }
    return h;
}

/// Sobrecarga con el mar de referencia. Existe para los llamadores que no participan del estado
/// (tests de la forma de la ola); el motor SIEMPRE debe pasar el estado que subió a la GPU.
inline float oceanWaveHeight(const glm::vec3& wp, const glm::vec3& up, float t,
                             float depthM, float fade = 1.0f) {
    static const OceanState s_ref = oceanDefaultState();
    return oceanWaveHeight(wp, up, t, depthM, fade, s_ref);
}

/**
 * @brief VELOCIDAD DEL AGUA en la superficie, m/s. Es lo que hace que el mar MUEVA las cosas.
 *
 * ⚠️ SIN ESTO LA FLOTACIÓN ES VERTICAL Y NADA LLEGA NUNCA A LA ORILLA. Un cuerpo con solo empuje de
 * Arquímedes sube y baja con la ola y se queda donde estaba: el mar lo mece pero no lo lleva. Lo que
 * transporta es el movimiento HORIZONTAL del agua, y en Gerstner no es un añadido — ya está en la
 * formulación, porque cada partícula describe un círculo.
 *
 * Es la derivada temporal EXACTA del desplazamiento de `harukaGerstner`:
 *
 *     disp = D·(Q·A·cos φ) + up·(A·sin φ)      con  φ = k·(D·wp) − ω·t
 *     ∂disp/∂t = D·(Q·A·ω·sin φ) − up·(A·ω·cos φ)
 *
 * No es una aproximación ni un modelo aparte: si la altura y la velocidad salen de la misma `disp`,
 * no pueden discrepar. Cambiar una constante de `OCEAN_WAVE` corrige las dos a la vez.
 *
 * La comprobación que importa: en la CRESTA (φ = π/2) queda `D·Q·A·ω`, puramente HORIZONTAL y en el
 * sentido de avance de la ola. Es la firma física del oleaje de Stokes —el agua de la cresta va hacia
 * delante y la del valle hacia atrás— y es, literalmente, lo que empuja un objeto flotante hacia la
 * playa.
 *
 * ⚠️ LÍMITE: es la velocidad EN LA SUPERFICIE. Bajo ella el movimiento orbital decae como `e^{k·z}`
 * (a media longitud de onda de profundidad ya es el 4 %), y aquí no se modela: quien flota está en la
 * superficie, que es el caso para el que se escribió. Un cuerpo SUMERGIDO recibirá de más — si algún
 * día hay buceo con arrastre, hay que atenuar por profundidad de inmersión.
 *
 * Parámetros idénticos a `oceanWaveHeight`, y el mismo `t` de `oceanClockSeconds()`.
 */
inline glm::vec3 oceanWaveVelocity(const glm::vec3& wp, const glm::vec3& up, float t,
                                   float depthM, float fade, const OceanState& st,
                                   float fetchM = WATER_FETCH_UNLIMITED) {
    // MISMO marco que `oceanWaveHeight` y que el shader. Si estas dos líneas divergen, la velocidad
    // apunta a otro sitio que la altura y el agua empuja en diagonal respecto a sus propias crestas.
    const glm::vec3 t1 = glm::normalize(std::abs(up.y) < 0.99f ? glm::cross(up, glm::vec3(0, 1, 0))
                                                               : glm::cross(up, glm::vec3(1, 0, 0)));
    const glm::vec3 t2 = glm::cross(up, t1);
    // MISMO bajío, MISMO tope común y MISMO escarpado que `oceanWaveHeight`. Si estas tres líneas se
    // separaran de las suyas, la velocidad dejaría de ser la derivada de la altura y el test de
    // paridad lo cazaría — que es justo para lo que está.
    const float green      = oceanGreenGain(depthM) * oceanFetchFactor(st, fetchM);
    const float scale      = oceanBreakScale(depthM, st, fetchM);
    const float breakiness = glm::clamp(1.0f - scale, 0.0f, 1.0f);
    glm::vec3 v(0.0f);
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        // Mismas tres líneas que `oceanWaveHeight`, y por el mismo motivo: si se separaran, la
        // velocidad dejaría de ser la derivada de la altura y `ocean_wave_velocity` lo cazaría.
        const float k0 = 6.2831853f / st.wave[i][0];
        const float k  = oceanWaveNumber(k0, depthM);
        const float amp = st.wave[i][1] * green * scale * fade * oceanShortWaveFade(k);
        if (amp <= 1e-4f) continue;
        const glm::vec3 D = glm::normalize(t1 * st.wave[i][2] + t2 * st.wave[i][3]);
        const float w  = std::sqrt(OCEAN_G * k0);         // la frecuencia NO cambia con el fondo
        const float ph = k * glm::dot(D, wp) - w * t;
        // Q = escarpado, GEMELO EXACTO de `ocean_wave.glsl`. Entra en la velocidad horizontal porque
        // es quien reparte cuánto del círculo es avance y cuánto subida — y al romper crece, que es
        // por lo que la cresta de una ola que revienta te empuja hacia la playa mucho más que el
        // mismo oleaje en agua honda.
        const float Q = oceanSteepness(k, amp, breakiness);
        v += D * (Q * amp * w * std::sin(ph)) - up * (amp * w * std::cos(ph));
    }
    return v;
}

/// Sobrecarga con el mar de referencia. Ver la nota de `oceanWaveHeight`.
inline glm::vec3 oceanWaveVelocity(const glm::vec3& wp, const glm::vec3& up, float t,
                                   float depthM, float fade = 1.0f) {
    static const OceanState s_ref = oceanDefaultState();
    return oceanWaveVelocity(wp, up, t, depthM, fade, s_ref);
}

}} // namespace Haruka::Planet
