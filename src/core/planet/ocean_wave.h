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
#include <cstdio>
#include <cstdlib>
#include "core/planet/water_fill.h"   // WATER_FETCH_UNLIMITED: el fetch del oceano

namespace Haruka { namespace Planet {

inline constexpr float  OCEAN_G     = 9.81f;
inline constexpr int    OCEAN_WAVES = 8;

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
/**
 * @brief ¿Está el agua APAGADA por `HARUKA_NOWATER=1`? Interruptor de biseccion.
 *
 * ⚠️ ESTA VARIABLE SE CITABA EN LAS NOTAS Y NO EXISTIA EN EL CODIGO. Se la recomende a Andoni para
 * bisecar "¿lo blanco es el agua?", corrio con ella, y no apago nada — o sea que su respuesta ("sigue
 * habiendo agua") no significaba NADA. Una sonda que no existe es peor que ninguna: devuelve algo con
 * pinta de medida.
 *
 * ⚠️ Y POR ESO SE ANUNCIA EN EL LOG la primera vez que se pregunta. Un interruptor mudo tiene el mismo
 * problema por otra puerta: si no apaga, no hay forma de distinguir "no hace efecto" de "no llego a
 * ejecutarse". Es la misma leccion que `HARUKA_REQUIRE_DEVICE`, que existe porque una cifra de GPU no
 * vale sin el nombre del dispositivo impreso al lado.
 */
inline bool oceanWaterDisabled() {
    static const bool s_off = [] {
        const char* e = std::getenv("HARUKA_NOWATER");
        const bool off = (e && e[0] == '1');
        std::fprintf(stderr, "[Ocean] HARUKA_NOWATER=%s -> agua %s\n",
                     e ? e : "(sin poner)", off ? "APAGADA" : "encendida");
        return off;
    }();
    return s_off;
}

/// Escala de espuma de `HARUKA_OCEAN_FOAM` (1 por defecto). Se anuncia igual, y por lo mismo.
inline float oceanFoamScale() {
    static const float s_scale = [] {
        const char* e = std::getenv("HARUKA_OCEAN_FOAM");
        const float v = e ? std::max(0.0f, (float)std::atof(e)) : 1.0f;
        std::fprintf(stderr, "[Ocean] HARUKA_OCEAN_FOAM=%s -> escala de espuma %.3f\n",
                     e ? e : "(sin poner)", v);
        return v;
    }();
    return s_scale;
}

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
    // ⚠️ ERAN CUATRO Y EL MAR SE REPETIA CADA 3,7 km. Medido antes de tocar nada
    // (`ocean_spectrum_limits`): autocorrelacion **0,983 a 3720 m** —o sea, a esa distancia se ve el
    // MISMO trozo de mar— y curtosis **2,115** contra los 3,00 de una gausiana. Un mar real es
    // gausiano por el teorema central del limite, y con cuatro senos no puede serlo.
    //
    // Los ocho NO estan escritos a mano: salen de muestrear **Pierson-Moskowitz** a U = 11,4 m/s
    // (`S(ω) = α g² ω⁻⁵ exp(−1,25 (ω_p/ω)⁴)`, α = 8,1e-3, ω_p = 0,877 g/U) en ocho longitudes, con
    // `aᵢ = sqrt(2·S(ωᵢ)·Δωᵢ)` y `Δω` entre los puntos medios de los vecinos.
    //
    // ⚠️ Y REESCALADOS PARA CONSERVAR LA ENERGIA EXACTA de la tabla vieja: `Hs = 4·sqrt(Σaᵢ²/2)` vale
    // **2,8021 m** antes y despues. Sin eso el mar habria CRECIDO al añadir trenes, y con el se
    // habrian movido el indice de rompiente, el fetch y la flotabilidad de la fisica — un cambio de
    // espectro se habria colado como un cambio de estado de mar.
    //
    // Las longitudes cubren de 8,7 m (el piso de Nyquist del quad, ver `oceanShortWaveFade`) a 137 m,
    // que es donde esta el pico de PM para este viento; antes se cortaba en 61 m y faltaba justo la
    // parte larga, que es la que da la variacion a gran escala. Los angulos ENSANCHAN con la
    // frecuencia, como en un mar real: los trenes largos casi con el viento, los cortos muy abiertos.
    { 137.0f, 0.5090f,  0.98481f, -0.17365f },   //  -10 grados
    {  89.0f, 0.5390f,  0.97030f,  0.24192f },   //  +14
    {  61.0f, 0.4394f,  1.00000f,  0.00000f },   //    0  (el tren que manda: sigue al viento)
    {  43.0f, 0.3492f,  0.86603f,  0.50000f },   //  +30
    {  29.0f, 0.2602f,  0.89879f, -0.43837f },   //  -26
    {  19.0f, 0.1748f,  0.57358f,  0.81915f },   //  +55
    {  12.7f, 0.1147f,  0.30902f, -0.95106f },   //  -72
    {   8.7f, 0.0742f, -0.30902f,  0.95106f },   // +108
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
/// Altura significativa del estado del mar (m): `Hs = 4·sqrt(Σaᵢ²/2)`.
inline float oceanSignificantHeight(const OceanState& st) {
    float sum2 = 0.0f;
    for (int i = 0; i < OCEAN_WAVES; ++i) sum2 += st.wave[i][1] * st.wave[i][1];
    return 4.0f * std::sqrt(sum2 * 0.5f);
}

/// El VIENTO que produce este estado (m/s), invirtiendo Pierson-Moskowitz (`Hs = 0,21·U²/g`).
/// ⚠️ Se DEDUCE del estado en vez de viajar en él: así no puede desincronizarse de la ola que
/// describe. Estaba escrito dentro de `oceanFetchFactor`; se saca porque ahora lo necesitan también
/// los borreguillos (`oceanWhitecapCoverage`) y el banco — y una tercera copia a mano era cuestión de
/// tiempo. Gemelo de `harukaWindSpeed`.
inline float oceanWindSpeed(const OceanState& st) {
    const float Hs = oceanSignificantHeight(st);
    return (Hs <= 1e-4f) ? 0.0f : std::sqrt(Hs * OCEAN_G / 0.21f);
}

/**
 * @brief Fracción de superficie cubierta de BORREGUILLOS en mar abierto, de Monahan &
 *        O'Muircheartaigh (1980): `W = 3,84e-6 · U^3,41`.
 *
 * No la consume la ola: es el ORÁCULO contra el que se calibra el umbral de espuma de cresta, y lo que
 * comprueba `ocean_whitecaps`. Se deja en el motor y no en el test a propósito — un número empírico
 * copiado en un test es un número que nadie vuelve a mirar.
 */
inline float oceanWhitecapCoverage(const OceanState& st) {
    const float U = oceanWindSpeed(st);
    return (U <= 0.0f) ? 0.0f : 3.84e-6f * std::pow(U, 3.41f);
}

inline float oceanFetchFactor(const OceanState& st, float fetchM) {
    // Océano, o sin dato: no se limita nada. `<= 0` es "este punto no trae fetch", no "fetch cero".
    if (fetchM <= 0.0f || fetchM >= WATER_FETCH_UNLIMITED * 0.5f) return 1.0f;
    const float Hs = oceanSignificantHeight(st);
    if (Hs <= 1e-4f) return 1.0f;
    const float U   = oceanWindSpeed(st);                       // el viento que da ese Hs (PM)
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
 * @brief REFRACCION por el fondo: la direccion del tren despues de que el bajio lo gire.
 *        Gemelo de `harukaRefract`.
 *
 * ⚠️ ESTO NO EXISTIA, Y ES LO QUE MAS SE NOTA DE UNA OLA REAL. `D` era la direccion 2D del tren
 * llevada al plano tangente y **no dependia del fondo en ninguna parte del motor** (ni CPU ni GLSL):
 * las olas llegaban a la playa con el angulo que trajeran del mar abierto. En el mar de verdad no
 * pasa nunca — al perder fondo la ola se frena, gira, y acaba entrando casi paralela a la orilla.
 * Es la razon por la que las crestas se ven paralelas a la playa desde cualquier punto de la costa.
 *
 * La ley es exacta y sale gratis con lo que ya hay: **el componente del vector de onda A LO LARGO de
 * la isobata se conserva** (Snell para ondas de agua, `k·sinθ = cte`). Con `k` ya calculado por
 * `oceanWaveNumber`:
 *
 *     k_along  = k0 · (D0 · along)          <- se conserva del mar abierto
 *     k_across = ±sqrt(k² − k_along²)       <- lo que queda, con el SIGNO original
 *
 * En agua profunda `k = k0` y sale `D0` exacto: el mar abierto no cambia ni un bit. Al perder fondo
 * `k` crece, `k_across` crece con el, y la direccion se endereza hacia la orilla.
 *
 * ⚠️ ES LA APROXIMACION LOCAL, no trazado de rayos. La refraccion de verdad es un efecto integrado a
 * lo largo del camino de la ola; aqui se aplica Snell con la profundidad y la pendiente DEL PUNTO.
 * Da la orientacion correcta de las crestas —que es lo que se ve— y no la historia del rayo.
 *
 * @param d0       direccion del tren en aguas profundas (unitaria, en el plano tangente).
 * @param upSlope  unitario en el plano tangente hacia agua MENOS profunda (gradiente del fondo).
 *                 El vector NULO significa "sin dato de pendiente" y no refracta nada.
 * @param up       normal local (radial).
 * @param k0,k     numero de onda profundo y local (`oceanWaveNumber`).
 */
inline glm::vec3 oceanRefract(const glm::vec3& d0, const glm::vec3& upSlope, const glm::vec3& up,
                              float k0, float k) {
    const float slopeLen = glm::length(upSlope);
    // Sin pendiente (fondo llano, o el llamante no la tiene) no hay nada que refractar. Y sin bajio
    // tampoco: en agua profunda la formula devuelve `d0` exacto, pero salir aqui ahorra la cuenta.
    if (slopeLen < 1.0e-6f || k <= 0.0f || k0 <= 0.0f) return d0;
    const glm::vec3 nS    = upSlope / slopeLen;
    const glm::vec3 along = glm::cross(up, nS);
    const float alongLen  = glm::length(along);
    if (alongLen < 1.0e-6f) return d0;
    const glm::vec3 aU = along / alongLen;

    const float a = glm::dot(d0, nS);      // componente hacia la orilla
    const float b = glm::dot(d0, aU);      // componente a lo largo de la isobata
    const float kAlong = k0 * b;           // ESTE es el que se conserva
    const float k2 = k * k - kAlong * kAlong;
    // `k2 < 0` = reflexion total interna: la ola no puede seguir cruzando y viaja por la isobata.
    // Solo pasa yendo hacia AGUA MAS HONDA (k < k0); el guardia esta por robustez, no por estetica.
    if (k2 <= 0.0f) return (b >= 0.0f) ? aU : -aU;
    const float kAcross = std::sqrt(k2) * ((a >= 0.0f) ? 1.0f : -1.0f);   // conserva el sentido
    const glm::vec3 v = nS * kAcross + aU * kAlong;
    const float vl = glm::length(v);
    return (vl > 1.0e-9f) ? (v / vl) : d0;
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
 * ⚠️ ESA NOTA DECIA "aqui lambda es fija" Y YA NO ES CIERTO: desde la dispersion finita, la Gerstner
 * usa `k = oceanWaveNumber(k0, d)` y la longitud SI se acorta al perder fondo (medido: el tren de
 * 61 m baja a 10,67 m con 30 cm de agua, x5,7 en `k`). Se deja escrito porque el comentario obsoleto
 * mandaba al siguiente lector a implementar algo que ya estaba.
 *
 * ⚠️⚠️ Y AUN ASI LA VOLUTA NO SALE — ahora con la cifra, que es lo que faltaba. Con la orbita eliptica
 * correcta (`oceanHorizAmp`) y barriendo el fondo de 5 cm a 20 m, el escarpado total `Σ k·horiz` que
 * decide el pliegue vale:
 *
 *     CON el tope comun   0,750  (a 5,20 m de fondo)
 *     SIN ningun tope     0,818  (a 5,15 m de fondo)      <- pliega si pasa de 1
 *
 * O sea que **no es el tope quien lo impide**: aunque se quitara entero, se queda en 0,818. Lo que lo
 * acota es el limite de rompiente (`oceanBreakScale` mantiene `Σ amp ≤ 0,55·d`), y con esa altura
 * frente a ese fondo la superficie sigue siendo una funcion. La voluta pide un modelo de ROMPIENTE
 * explicito —una asimetria hacia delante en el punto de romper— y no un parametro de Gerstner. Ver
 * `oceanHorizAmp` y el test `ocean_gerstner_ellipse`, que imprime las dos cifras en cada pasada.
 */
inline float oceanSteepness(float k, float amp, float /*breakiness*/) {
    // Reparto del escarpado entre los trenes para que la suma no se pliegue sola en mar abierto.
    return std::min(0.75f / (k * amp * float(OCEAN_WAVES) + 1e-4f), 1.0f);
}

/// Techo del escarpado TOTAL. Por debajo de 1 el jacobiano no puede volverse negativo, o sea que la
/// lamina no puede plegarse sobre si misma. Es el mismo 0,75 que imponia `oceanSteepness`, conservado
/// A PROPOSITO: la formula nueva no puede quedar MAS escarpada que la vieja en ningun punto, asi que
/// la regresion que obligo a revertir la voluta (el agua interior plegandose sobre celdas de 4,4 m)
/// no puede volver por esta via.
inline constexpr float OCEAN_MAX_STEEP = 0.75f;

/**
 * @brief Semieje HORIZONTAL de la orbita de Gerstner en fondo finito (m). Gemelo de `harukaHorizAmp`.
 *
 * ⚠️ AQUI ESTABA EL BUG DE VERDAD, Y NO ERA LA VOLUTA. El desplazamiento horizontal era `Q·A`, con
 * `Q = 0.75/(k·A·N)`, o sea **`0.75/(k·N)`: NO DEPENDIA DE LA AMPLITUD**. Una charca de 30 cm recibia
 * el mismo vaiven horizontal que el mar abierto, porque la amplitud se cancelaba en la propia
 * formula. Medido con la tabla de referencia en agua profunda, el reparto que salia:
 *
 *     λ = 61,0 m · A = 0,85 m -> horizontal 1,82 m   (2,1x la amplitud)
 *     λ =  8,7 m · A = 0,09 m -> horizontal 0,26 m   (2,9x la amplitud)
 *
 * La orbita de una particula en una ola de Gerstner es una ELIPSE de semiejes `A/tanh(k·d)`
 * (horizontal) y `A` (vertical). En agua profunda `tanh(k·d) -> 1` y la orbita es un CIRCULO: el
 * horizontal vale `A`, no 2-3 veces `A`. En somero `tanh(k·d) -> k·d` y el horizontal crece — que es
 * lo que de verdad pasa: en poca agua te mueve de lado mucho mas que arriba y abajo.
 *
 * Y lo importante: **escala con `A`**. Ese es el motivo por el que el intento anterior de la voluta
 * rompio el agua interior y este no puede: alli el horizontal era una constante en metros, aqui es
 * proporcional a la ola que de verdad hay en ese punto.
 */
inline float oceanHorizAmp(float k, float amp, float depthM) {
    // El suelo del `tanh` evita dividir por cero justo en la orilla (d -> 0). No es un ajuste
    // estetico: sin el, un texel con profundidad exactamente 0 produce un NaN que se propaga a la
    // posicion del vertice y se lleva por delante el parche entero.
    const float th = std::max(std::tanh(k * std::max(depthM, 0.0f)), 1.0e-3f);
    return amp / th;
}

/**
 * @brief Cuanto se ADELANTA la cresta al romper, como fraccion del semieje horizontal.
 *        Gemelo de `harukaCurlGain`.
 *
 * ⚠️ POR QUE HACE FALTA ESTO Y NO OTRO PARAMETRO DE GERSTNER. Medido: con la orbita eliptica correcta
 * y barriendo el fondo de 5 cm a 20 m, el escarpado total `Σ k·horiz` llega como mucho a **0,818**, y
 * plegar pide pasar de 1. Quitando el tope comun entero se queda igual — o sea que **no es el tope
 * quien lo impide**, es el limite de rompiente (`Σ amp ≤ 0,55·d`). Una ola simetrica con esa altura
 * frente a ese fondo sencillamente no se pliega, por mucho que se le suba el escarpado. Ese callejon
 * ya costo un intento revertido.
 *
 * Lo que le falta a la ola no es escarpado: es **ASIMETRIA**. Una ola que rompe no es una sinusoide
 * alta — tiene la cara delantera casi vertical y la trasera tendida, y la cresta se adelanta al resto
 * de la columna. Es el termino de segundo orden que aparece al crecer la no linealidad en somero
 * (asimetria de Stokes/cnoidal); ignorarlo es lo que deja la ola con cara de seno hasta el final.
 *
 * Aqui se anade como un empujon EXTRA del desplazamiento horizontal concentrado en la cresta:
 * `horiz · (1 + curl·max(0, cos φ))`. Fuera de la cresta (`cos φ ≤ 0`) no toca nada, asi que el seno
 * del mar abierto no cambia. `curl` sale de `oceanBreakiness`, que es exactamente "el limite de
 * rompiente esta mordiendo".
 *
 * ⚠️ Y ES PROPORCIONAL A LA AMPLITUD, que es lo que salva al agua interior. El intento revertido
 * fallo porque su desplazamiento horizontal era una constante en metros: una charca de 30 cm recibia
 * los mismos ~3 m que el oceano y la lamina se plegaba sobre celdas de 4,4 m. Aqui el empujon es una
 * fraccion de `horiz`, que ya escala con `A` — una ola de 10 cm se adelanta centimetros.
 */
inline constexpr float OCEAN_CURL_MAX = 1.10f;   ///< adelanto maximo de la cresta (x el semieje)

/**
 * @brief INDICE DE ROMPIENTE `H/d`: la altura que la ola QUIERE tener (antes del tope) contra el
 *        fondo que hay. Gemelo de `harukaBreakIndex`.
 *
 * Es la variable clasica que decide si una ola rompe: revienta al pasar de **~0,78**. Se calcula con
 * la altura SIN recortar a proposito — el tope (`oceanBreakScale`) es lo que la aplasta, asi que
 * preguntarle a la altura ya recortada si esta rompiendo es circular.
 */
inline float oceanBreakIndex(float depthM, const OceanState& st,
                             float fetchM = WATER_FETCH_UNLIMITED) {
    if (!(depthM > 1.0e-4f)) return 0.0f;
    const float green = oceanGreenGain(depthM) * oceanFetchFactor(st, fetchM);
    float sum = 0.0f;
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        const float k0 = 6.2831853f / st.wave[i][0];
        sum += st.wave[i][1] * oceanShortWaveFade(oceanWaveNumber(k0, depthM));
    }
    return 2.0f * sum * green / depthM;     // H = 2·Σamp (pico a pico)
}

inline float oceanCurlGain(float depthM, const OceanState& st,
                           float fetchM = WATER_FETCH_UNLIMITED) {
    // ⚠️ AQUI HABIA `breakiness²` Y CAIA EN EL SITIO EQUIVOCADO. `breakiness` solo despega cuando el
    // tope MUERDE, y el tope esta puesto en `Σamp ≤ 0,55·d`, o sea en `H/d = 1,1`. Romper empieza en
    // 0,78, asi que la asimetria no aparecia hasta muy pasado el punto de rompiente: medido, la
    // franja que plegaba caia en **0,05–1,65 m** cuando una ola de 2,80 m de Hs rompe hacia **3,6 m**.
    // Estaba rompiendo en la orilla misma en vez de en la barra.
    //
    // Ahora sale del indice de rompiente de verdad. Los dos numeros tienen procedencia y no son un
    // ajuste: **0,78** es el indice clasico al que una ola revienta, y **1,1** es donde muerde el tope
    // propio del motor. Se arranca un poco antes (0,60) porque la cara delantera se empina ANTES de
    // plegarse — que es lo que hace que una ola se vea "a punto" antes de romper.
    const float gamma = oceanBreakIndex(depthM, st, fetchM);
    return OCEAN_CURL_MAX * glm::smoothstep(0.60f, 1.10f, gamma);
}

/**
 * @brief Refuerzo DIRECCIONAL de la rompiente: cuánto se le suma al semieje horizontal del tren que
 *        rompe, como fracción de sí mismo. Gemelo de `harukaBreakerGain`.
 *
 * ⚠️ ESTO ES LO QUE FALTABA PARA QUE UNA OLA VUELQUE, y no era un problema de "cuánta" compresión sino
 * de DÓNDE. Medido con la derivada numérica del desplazamiento, los autovalores del mapa horizontal en
 * el peor punto de una playa 1:20:
 *
 *     fondo 8,0 m -> λ = 0,286 y 0,552   ·  fondo 5,0 m -> λ = 0,192 y 0,722
 *
 * El escarpado por divergencia ya pasaba de 1 (`max(1−jac) = 1,162` a 8 m), o sea que compresión
 * sobraba. Pero se REPARTÍA entre las dos direcciones, y un determinante no se anula así: hace falta
 * que UN autovalor llegue a 0. La causa es que la refracción alinea los trenes sólo en parte —un tren
 * oblicuo de 70° entra a 38° con 5 m de fondo, no a 0°— así que ocho trenes en un abanico comprimen en
 * todas las direcciones a la vez.
 *
 * La solución es la que usa el mar de verdad: en la zona de rompientes el oleaje se organiza en
 * **crestas paralelas a la orilla**, y toda la compresión cae en un eje. Aquí eso se hace reforzando
 * el semieje del **tren 0 refractado** —el mismo que `oceanSwash` toma como "el tren que rompe"—, que
 * al perder fondo ya apunta a la orilla. No es un tren nuevo ni energía nueva: es concentrar la
 * asimetría donde el agua la concentra.
 *
 * ⚠️ Y LAS DOS PUERTAS POR LAS QUE ENTRÓ EL FALLO DE 2026-08-29 SIGUEN CERRADAS. Aquel intento metía
 * un desplazamiento horizontal que NO dependía de la amplitud: los mismos ~3 m en un lago de 30 cm que
 * en mar abierto, y la lámina se plegaba sobre celdas de 4,4 m. Esto es una FRACCIÓN de `oceanHorizAmp`
 * del propio tren, que ya escala con `A`; y está cerrado por el índice de rompiente, que arranca en el
 * 0,78 clásico. Medido en una charca de 30 cm con fetch de 100 m: el desplazamiento horizontal máximo
 * es de 0,785 m ANTES de esto, y el refuerzo es proporcional a él.
 */
inline constexpr float OCEAN_BREAKER_MAX = 1.60f;

inline float oceanBreakerGain(float depthM, const OceanState& st,
                              float fetchM = WATER_FETCH_UNLIMITED) {
    // Arranca en el 0,78 clásico —el índice al que una ola revienta— y no en el 0,60 del adelanto de
    // cresta: peraltarse es una cosa y volcar es otra, y no deben empezar juntas.
    const float gamma = oceanBreakIndex(depthM, st, fetchM);
    return OCEAN_BREAKER_MAX * glm::smoothstep(0.78f, 1.20f, gamma);
}

/**
 * @brief Factor COMUN que impide que la lamina se pliegue sobre si misma. Gemelo de `harukaSteepScale`.
 *
 * Mismo criterio que `oceanBreakScale` y por el mismo motivo: se encoge la ola ENTERA conservando su
 * forma, en vez de recortarle el escarpado a unos trenes y no a otros (que daria un mar de otro
 * aspecto segun la profundidad). El escarpado total es `Σ k·horiz`; el jacobiano es `1 - Σ k·horiz·sin φ`,
 * asi que mientras la suma no pase de 1 la superficie es una funcion y no se puede plegar.
 */
/**
 * @brief Coeficiente del SEGUNDO ARMÓNICO: cresta picuda y seno plano. Gemelo de `harukaSkewness`.
 *
 * ⚠️ SIN ESTO EL PERFIL DE LA OLA ES UN SENO PURO INCLUSO ROMPIENDO, y es lo que más delata a un mar
 * sintético. `oceanCurlGain` sólo toca el desplazamiento HORIZONTAL; la elevación seguía siendo
 * `A·sin φ`, o sea perfectamente simétrica: crestas y senos con la misma forma. Una ola de bajío real
 * es cnoidal — cresta estrecha y alta, seno ancho y poco profundo.
 *
 * Es el segundo armónico de Stokes. Con la cresta en `φ = π/2` (el convenio de este fichero, donde la
 * elevación es `A·sin φ`), el término es `−S·A·cos 2φ`:
 *
 *     φ = π/2  (cresta) -> −S·A·cos π    = +S·A     la cresta SUBE
 *     φ = −π/2 (seno)   -> −S·A·cos(−π)  = +S·A     el seno SUBE (es menos hondo)
 *     φ = 0    (paso)   -> −S·A·cos 0    = −S·A
 *
 * ⚠️ **Y NO CAMBIA LA ALTURA PICO-VALLE**, que es la propiedad que lo hace seguro: la cresta pasa a
 * `A(1+S)` y el seno a `−A(1−S)`, cuya diferencia sigue siendo `2A`. Así el límite de rompiente
 * (`Σ amp ≤ 0,55·d`), el índice de rompiente y la flotabilidad siguen significando lo mismo — la ola
 * cambia de FORMA sin cambiar de tamaño. Su media en un periodo también es 0: no mueve el nivel.
 *
 * ⚠️ Y ES VERTICAL, así que **no puede plegar la lámina sobre sí misma**. El intento de voluta que
 * hubo que revertir metía metros de desplazamiento HORIZONTAL en una charca de 30 cm; esto no toca la
 * horizontal, y además es una fracción de la amplitud, o sea que una ola de 10 cm se peralta
 * centímetros. Las dos puertas por las que entró aquel fallo están cerradas.
 *
 * Arranca ANTES que el adelanto de cresta (0,30 contra 0,60 del índice de rompiente): una ola se
 * peralta al entrar en el bajío mucho antes de estar a punto de romper.
 */
/**
 * ⚠️ EL TOPE DE 0,25 NO ES UN AJUSTE: ES DONDE EL PERFIL DEJA DE SER UNA OLA. Con
 * `η = a·(sin φ − S·cos 2φ)`, la derivada es `a·cos φ·(1 + 4S·sin φ)`, que se anula en `cos φ = 0`
 * (cresta y seno) y además en `sin φ = −1/(4S)`. Ese segundo par de extremos **existe en cuanto
 * `S > 0,25`**: al seno le sale un montículo secundario en medio, que es una ola de mentira.
 *
 * ⚠️ Y EL VALOR CONSTANTE QUE HABÍA ERA 0,30, o sea YA POR ENCIMA del límite. El bajío estaba
 * dibujando ese montículo. Se mide en `ocean_skewness` contando extremos por periodo.
 */
inline constexpr float OCEAN_SKEW_MAX = 0.25f;

/// Banda de escarpado local (`1 − jacobiano`) en la que aparece la espuma de CRESTA. No son numeros
/// elegidos: el punto medio (0,21) es el percentil del escarpado que deja la cobertura de Monahan
/// (1,56 % a 11,44 m/s) en mar abierto. Ver `oceanFoam` y el test `ocean_whitecaps`.
/// ⚠️ Gemelos de `HARUKA_WHITECAP_LO` / `HARUKA_WHITECAP_HI`.
/// Ajuste que liga la cola de la distribucion de pendientes con la cobertura empirica. Si `|∇η|` fuera
/// exactamente gaussiano isotropo, el umbral que deja una fraccion `W` por encima seria
/// `σ·sqrt(−2·ln W)`; los ocho trenes no cubren todas las direcciones, asi que la cola real es mas
/// estrecha y hace falta este factor. Medido: el percentil 98,44 de la pendiente es 0,186 con
/// `σ = 0,0981`, o sea `1,897·σ` frente al `2,885·σ` del gaussiano -> 0,657.
inline constexpr float OCEAN_WHITECAP_FIT = 0.657f;
/// Semiancho de la banda alrededor del umbral (el `smoothstep` no puede ser un escalon).
inline constexpr float OCEAN_WHITECAP_LO = 0.769f;
inline constexpr float OCEAN_WHITECAP_HI = 1.250f;

/**
 * @brief Coeficiente del 2º armónico de UN TREN, del STOKES de segundo orden. Gemelo de
 *        `harukaStokesSkew`.
 *
 * ⚠️ ANTES ERA UNA CONSTANTE (0,30) CON UNA PUERTA POR PROFUNDIDAD, o sea un número elegido a mano y
 * el MISMO para los ocho trenes. Eso no es Stokes: la asimetría de una ola depende de SU escarpado
 * (`k·a`) y de SU relación con el fondo (`k·d`), así que un tren largo en 2 m de agua tiene que
 * peraltarse mucho y uno corto casi nada. Con una constante común, los ocho se peraltaban igual.
 *
 * La expresión es la clásica de segundo orden en profundidad finita:
 *
 *     a₂ = (k·a²/4) · cosh(kd)·(2 + cosh 2kd) / sinh³(kd)      ⇒   S = a₂/a
 *
 * Y cumple los dos límites, que es lo que la hace comprobable y no un ajuste:
 *  · aguas PROFUNDAS (`kd → ∞`): el factor tiende a 2 y queda `S = k·a/2`, el Stokes de toda la vida.
 *  · aguas SOMERAS (`kd → 0`): el factor va como `3/(kd)³`, o sea el número de Ursell — y es por lo
 *    que Stokes DEJA DE VALER en agua muy somera (allí manda la teoría cnoidal). De ahí que haga
 *    falta el tope, que además tiene su propia derivación (ver `OCEAN_SKEW_MAX`).
 */
inline float oceanStokesSkew(float k, float amp, float depthM) {
    if (!(k > 0.0f) || !(amp > 0.0f)) return 0.0f;
    const float kd = k * std::max(depthM, 0.02f);
    // Aguas profundas: `cosh`/`sinh` desbordan y el cociente vale 2 con precisión de float mucho
    // antes. La salida rápida no es una optimización, es lo que evita el `inf/inf`.
    const float f = (kd >= 10.0f) ? 2.0f
                                  : std::cosh(kd) * (2.0f + std::cosh(2.0f * kd))
                                    / std::pow(std::sinh(kd), 3.0f);
    return std::min(0.25f * k * amp * f, OCEAN_SKEW_MAX);
}

/// Asimetría REPRESENTATIVA del estado del mar a esta profundidad (el tren dominante). No la usa la
/// ola —cada tren lleva la suya, ver `oceanStokesSkew`— pero sí el banco y el diagnóstico.
inline float oceanSkewness(float depthM, const OceanState& st,
                           float fetchM = WATER_FETCH_UNLIMITED) {
    const float green = oceanGreenGain(depthM) * oceanFetchFactor(st, fetchM);
    const float scale = oceanBreakScale(depthM, st, fetchM);
    float best = 0.0f, bestAmp = 0.0f;
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        if (!(st.wave[i][0] > 0.0f)) continue;
        const float k0 = 6.2831853f / st.wave[i][0];
        const float k  = oceanWaveNumber(k0, depthM);
        const float amp = st.wave[i][1] * green * scale * oceanShortWaveFade(k);
        if (amp > bestAmp) { bestAmp = amp; best = oceanStokesSkew(k, amp, depthM); }
    }
    return best;
}

inline float oceanSteepScale(float depthM, const OceanState& st,
                             float fetchM = WATER_FETCH_UNLIMITED) {
    const float green = oceanGreenGain(depthM) * oceanFetchFactor(st, fetchM);
    const float brk   = oceanBreakScale(depthM, st, fetchM);
    float sum = 0.0f;
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        const float k0 = 6.2831853f / st.wave[i][0];
        const float k  = oceanWaveNumber(k0, depthM);
        const float amp = st.wave[i][1] * green * brk * oceanShortWaveFade(k);
        sum += k * oceanHorizAmp(k, amp, depthM);
    }
    // ⚠️ EL TECHO SUBE DONDE LA OLA ROMPE, y ese es el punto entero. Con el techo fijo en 0,75 la
    // cresta no puede plegarse NUNCA, que es lo que se midio. Donde `breakiness` dice que el limite
    // de rompiente esta mordiendo —o sea donde una ola de verdad revienta— se le deja pasar de 1.
    // Fuera de esa franja el techo sigue siendo el de antes, asi que el mar abierto y el agua
    // interior quieta no cambian ni un bit.
    const float ceiling = OCEAN_MAX_STEEP
                        + (1.0f + OCEAN_CURL_MAX - OCEAN_MAX_STEEP) * oceanCurlGain(depthM, st, fetchM)
                          / std::max(OCEAN_CURL_MAX, 1e-6f);
    return (sum > ceiling && sum > 1e-6f) ? (ceiling / sum) : 1.0f;
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
 * @brief DESPLAZAMIENTO COMPLETO de Gerstner (m), el valor que DEVUELVE `harukaGerstner`.
 *
 * ⚠️ ESTE GEMELO FALTABA Y POR ESO NO HABIA ORACULO. La CPU solo sabia devolver la componente
 * VERTICAL (`oceanWaveHeight`), asi que no habia forma de comprobar el jacobiano contra nada: la
 * unica referencia era la propia acumulacion analitica, que es lo que se queria verificar. Con el
 * desplazamiento entero, el jacobiano se puede medir por DIFERENCIAS FINITAS y la formula deja de ser
 * su propio juez.
 *
 * Es tambien lo que hace falta para saber donde esta de verdad la cresta: el vaiven horizontal es el
 * que afila la ola, y sin el la "posicion" de un punto de la superficie es la de la lamina en reposo.
 *
 * Parámetros idénticos a `oceanWaveHeight`.
 */
inline glm::vec3 oceanDisplacement(const glm::vec3& wp, const glm::vec3& up, float t,
                                   float depthM, float fade, const OceanState& st,
                                   float fetchM = WATER_FETCH_UNLIMITED,
                                   const glm::vec3& upSlope = glm::vec3(0.0f)) {
    const glm::vec3 t1 = glm::normalize(std::abs(up.y) < 0.99f ? glm::cross(up, glm::vec3(0, 1, 0))
                                                               : glm::cross(up, glm::vec3(1, 0, 0)));
    const glm::vec3 t2 = glm::cross(up, t1);
    const float green      = oceanGreenGain(depthM) * oceanFetchFactor(st, fetchM);
    const float scale      = oceanBreakScale(depthM, st, fetchM);
    const float steepScale = oceanSteepScale(depthM, st, fetchM);
    const float curl       = oceanCurlGain(depthM, st, fetchM);
    const float breaker    = oceanBreakerGain(depthM, st, fetchM);
    glm::vec3 disp(0.0f);
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        if (!(st.wave[i][0] > 0.0f)) continue;
        const float k0 = 6.2831853f / st.wave[i][0];
        const float k  = oceanWaveNumber(k0, depthM);
        const float amp = st.wave[i][1] * green * scale * fade * oceanShortWaveFade(k);
        if (amp <= 1e-4f) continue;
        const glm::vec3 D = oceanRefract(glm::normalize(t1 * st.wave[i][2] + t2 * st.wave[i][3]),
                                         upSlope, up, k0, k);
        const float w  = std::sqrt(OCEAN_G * k0);
        const float ph = k * glm::dot(D, wp) - w * t;
        const float c = std::cos(ph), s = std::sin(ph);
        // ⚠️ EL REFUERZO DE ROMPIENTE VA SOLO AL TREN 0, Y ESE ES EL PUNTO. Repartido entre los ocho
        // comprime en ocho direcciones y el determinante nunca llega a 0; concentrado en el tren que
        // rompe —refractado, o sea ya apuntando a la orilla— toda la compresión cae en un eje, que es
        // como se organiza un surf real. Ver `oceanBreakerGain`.
        const float horiz = oceanHorizAmp(k, amp, depthM) * steepScale
                          * (1.0f + curl * std::max(0.0f, c))
                          * ((i == 0) ? (1.0f + breaker) : 1.0f);
        // El 2º armónico va en la VERTICAL: cresta picuda, seno plano. POR TREN, del escarpado y el
        // fondo de ESE tren (`oceanStokesSkew`), no de una constante común a los ocho.
        const float skew = oceanStokesSkew(k, amp, depthM);
        disp += D * (horiz * c) + up * (amp * (s - skew * std::cos(2.0f * ph)));
    }
    return disp;
}

/**
 * @brief JACOBIANO del desplazamiento de Gerstner en este punto. Gemelo de la `jac` que acumula
 *        `harukaGerstner`. `1` = lámina sin deformar · `<0` = superficie PLEGADA, o sea rompiendo.
 *
 * ⚠️ ESTO NO EXISTIA EN CPU Y SU AUSENCIA TENIA UN COSTE CONCRETO: `ocean_break_fold` reimplementaba
 * la cuenta A MANO, y su copia se quedó anclada a la cadena vieja (`oceanSteepness` como `Q` y
 * `k = 2π/λ` de aguas profundas, sin `oceanWaveNumber`, sin `oceanHorizAmp`, sin `oceanSteepScale` y
 * sin `oceanCurlGain`). El test seguía verde midiendo un motor que ya no existe — el mismo modo de
 * fallo que `terrain_two_bakes_disagree`. Con el jacobiano aquí, el test no puede volver a
 * desincronizarse: llama a lo que llama el motor.
 *
 * `jac = 1 − Σ k·horiz·sin φ`, con `horiz` el semieje horizontal REAL (elipse de Gerstner en fondo
 * finito, escalada por el techo de escarpado y con el adelanto de cresta). Es la misma línea del
 * shader, término a término.
 *
 * Parámetros idénticos a `oceanWaveHeight`.
 */
inline float oceanJacobian(const glm::vec3& wp, const glm::vec3& up, float t,
                           float depthM, float fade, const OceanState& st,
                           float fetchM = WATER_FETCH_UNLIMITED,
                           const glm::vec3& upSlope = glm::vec3(0.0f),
                           float* outSlope = nullptr, float* outSigma = nullptr) {
    // MISMO marco tangente que `oceanWaveHeight`, `oceanWaveVelocity` y el shader.
    const glm::vec3 t1 = glm::normalize(std::abs(up.y) < 0.99f ? glm::cross(up, glm::vec3(0, 1, 0))
                                                               : glm::cross(up, glm::vec3(1, 0, 0)));
    const glm::vec3 t2 = glm::cross(up, t1);
    const float green      = oceanGreenGain(depthM) * oceanFetchFactor(st, fetchM);
    const float scale      = oceanBreakScale(depthM, st, fetchM);
    const float steepScale = oceanSteepScale(depthM, st, fetchM);
    const float curl       = oceanCurlGain(depthM, st, fetchM);
    const float breaker    = oceanBreakerGain(depthM, st, fetchM);
    float jac = 1.0f;
    // ⚠️ LA PENDIENTE DE LA SUPERFICIE, acumulada en este mismo bucle (no cuesta otra pasada). Es
    // `∇(Σ amp·sin φ) = Σ D·(amp·k·cos φ)`, o sea el escarpado de VERDAD — y NO es lo mismo que
    // `1−jac`, que es la divergencia del mapa horizontal e incluye el `1/tanh(k·d)` de la elipse
    // orbital. Ese factor se dispara en el bajio (σ pasa de 0,098 a 0,268 a 3 m) sin que la ola sea
    // mas escarpada: la orbita se ENSANCHA, que es otra cosa. La pendiente sube a 0,186 a 8 m y
    // vuelve a BAJAR (0,123 a 3 m) cuando el limite de rompiente aplasta la amplitud, que es lo que
    // de verdad hace una ola al romper. La usa `oceanFoam` para los borreguillos.
    glm::vec2 grad(0.0f);
    float sumSq = 0.0f;
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        if (!(st.wave[i][0] > 0.0f)) continue;          // guardia de `λ > 0`, ver `oceanWaveHeight`
        const float k0 = 6.2831853f / st.wave[i][0];
        const float k  = oceanWaveNumber(k0, depthM);
        const float amp = st.wave[i][1] * green * scale * fade * oceanShortWaveFade(k);
        if (amp <= 1e-4f) continue;
        const glm::vec3 D = oceanRefract(glm::normalize(t1 * st.wave[i][2] + t2 * st.wave[i][3]),
                                         upSlope, up, k0, k);
        const float w  = std::sqrt(OCEAN_G * k0);       // la frecuencia NO cambia con el fondo
        const float ph = k * glm::dot(D, wp) - w * t;
        const float c = std::cos(ph), s = std::sin(ph);
        // ⚠️ AQUI HABIA UN ERROR DE DERIVADA, Y ERA EL QUE MANTENIA APAGADA LA ESPUMA DE PLEGADO.
        // El desplazamiento a lo largo de `D` es `H·cos φ·(1 + g·max(0,cos φ))`, o sea
        // `H·(cos φ + g·cos²φ)` en la mitad delantera. Su derivada respecto a la fase es
        // `−H·sin φ·(1 + 2g·cos φ)` — con un DOS, porque el termino del adelanto es cuadratico en
        // `cos φ`. El motor acumulaba `horiz·k·sin φ`, o sea `(1 + g·cos φ)`: se dejaba la mitad del
        // efecto del adelanto justo en la magnitud que decide si la ola rompe.
        //
        // Medido con la derivada NUMERICA de `oceanDisplacement` como oraculo: el peor
        // `|analitico − traza|` era **0,3071**. No es un ajuste fino: es que la formula no era la
        // derivada de lo que el motor dibuja.
        const float dHoriz = oceanHorizAmp(k, amp, depthM) * steepScale
                           * (1.0f + 2.0f * curl * std::max(0.0f, c))
                           * ((i == 0) ? (1.0f + breaker) : 1.0f);   // ver `oceanBreakerGain`
        jac -= dHoriz * k * s;
        const float ka = amp * k;
        grad += glm::vec2(glm::dot(D, t1), glm::dot(D, t2)) * (ka * c);
        sumSq += ka * ka;                      // σ de la pendiente: escala de la distribucion
    }
    if (outSlope) *outSlope = glm::length(grad);
    if (outSigma) *outSigma = std::sqrt(sumSq * 0.5f);
    return jac;
}

/**
 * @brief ESPUMA en este punto, 0..1. Gemelo EXACTO de `outFoam` de `harukaGerstner`.
 *
 * ⚠️ ESTE GEMELO FALTABA, Y ERA LA UNICA MAGNITUD DE LA OLA SIN NINGUNA COBERTURA DE PARIDAD. La
 * altura, la velocidad, el número de onda, la refracción y el límite de rompiente tienen careo
 * CPU↔GPU; la espuma se calculaba SOLO en el shader, así que no había con qué carearla. Y la
 * consecuencia no es solo de test: sin este lado, la física y el juego no pueden preguntar "¿esto es
 * agua blanca?", o sea que una ola que revienta lo hace únicamente para la cámara — no hay arrastre
 * de la rompiente, ni sonido posicional, ni nada que el jugador note al meterse.
 *
 * Dos vías que se refuerzan, y se toma la MAYOR (no la suma: dos causas de espuma en el mismo punto
 * no dan el doble de aire):
 *  · **plegado** — `jac` bajo: la cresta se ha sobre-escarpado y vuelca. Es la definición geométrica
 *    de romper, y vale donde ocurra: en la barra de la playa o en mar abierto con viento.
 *  · **orilla** — la ola alcanza su límite contra el fondo y revienta entera. Se mide en unidades de
 *    la propia ola (`d / 2A₀`), no en metros, para que un mar grueso espume más lejos que uno tendido.
 *
 * ⚠️ LOS CUATRO UMBRALES SON LITERALES GEMELOS de `ocean_wave.glsl`. No se derivan de nada: son la
 * forma que se eligió para el desvanecido. Si se tocan aquí hay que tocarlos allí, y `ocean_foam` lo
 * caza (compara los dos lados en 2 400 muestras).
 *
 * ⚠️ `smoothstep` CON LOS BORDES INVERTIDOS (`e0 > e1`) es, por especificación, COMPORTAMIENTO
 * INDEFINIDO en GLSL. Todas las implementaciones reales lo calculan como
 * `t = clamp((x−e0)/(e1−e0), 0, 1)`, que es exactamente lo que hace `glm::smoothstep`, así que los
 * dos lados coinciden hoy en los dos backends — pero es una apuesta sobre el driver, no una garantía
 * del lenguaje. Anotado en TODO.md.
 *
 * Parámetros idénticos a `oceanWaveHeight`.
 */
inline float oceanFoam(const glm::vec3& wp, const glm::vec3& up, float t,
                       float depthM, float fade, const OceanState& st,
                       float fetchM = WATER_FETCH_UNLIMITED,
                       const glm::vec3& upSlope = glm::vec3(0.0f)) {
    float slope = 0.0f, sigma = 0.0f;
    const float jac = oceanJacobian(wp, up, t, depthM, fade, st, fetchM, upSlope, &slope, &sigma);
    (void)jac;
    // ⚠️ ESTA RAMA ERA `smoothstep(0.55, 0.05, jac)` Y ESTABA MUERTA: pedía un escarpado local de
    // 0,45 y en mar abierto se alcanza el **0,000 %** de las veces (200 000 muestras). El océano salía
    // liso y oscuro hasta la costa, cuando un mar de 11,4 m/s tiene crestas blancas.
    //
    // Ahora el umbral no es un número elegido: se CALIBRA contra Monahan & O'Muircheartaigh
    // (`oceanWhitecapCoverage`, `W = 3,84e-6·U^3,41`). Con el viento que el propio estado deduce
    // (11,44 m/s) toca **1,562 %** de cobertura, y el percentil correspondiente del escarpado local
    // medido es **0,208** — de ahí la banda 0,16–0,26, cuyo punto medio cae ahí. Medido después:
    //
    //     mar abierto (60-400 m)  1,466 %   <- contra el 1,562 % de Monahan
    //     20 m   5,2 %  ·  8 m  25,6 %  ·  2 m  22,5 %   <- sube sola hacia la rompiente
    //
    // Y absorbe a la rama vieja: `foldFoam` sólo se encendía con un escarpado de 0,45, o sea muy
    // dentro de lo que esta banda ya satura. Es la misma magnitud con un umbral que sale de un dato.
    // ⚠️ EL CRITERIO ES LA PENDIENTE DE LA SUPERFICIE, Y EL UMBRAL SE DERIVA DE LA COBERTURA EMPIRICA.
    // Llegar aqui costo tres intentos, y los tres fallos estan medidos:
    //  1. `1−jac` con umbral ABSOLUTO: calibra en mar abierto (1,47 % contra el 1,56 % de Monahan)
    //     pero **blanquea la costa** — la espuma media a 8 m se disparaba a 0,283. `1−jac` lleva
    //     dentro el `1/tanh(k·d)` de la elipse orbital, que en el bajio ensancha la orbita sin que la
    //     ola sea mas escarpada. La divergencia NO es el escarpado.
    //  2. lo mismo normalizado por su σ: arregla la costa y **mata la dependencia del viento**.
    //  3. la PENDIENTE con umbral absoluto: correcta en mar abierto, pero la cola de una gaussiana es
    //     exponencialmente sensible y la cobertura se disparaba x18,7 con x1,27 de viento.
    //
    // La salida es no elegir el umbral sino DERIVARLO: se conoce la cobertura que toca
    // (`oceanWhitecapCoverage`, Monahan) y se conoce la escala de la distribucion (σ, acumulada en el
    // mismo bucle), asi que el umbral sale de invertir la cola. La cobertura pasa a ser Monahan POR
    // CONSTRUCCION a cualquier viento — y como el mar de Pierson-Moskowitz de este motor escala
    // amplitud y longitud de onda a la vez (`oceanStateFromWind`, las dos con `U²`), su escarpado es
    // invariante con el viento y NINGUN criterio de pendiente podria haber reproducido `U^3,41` solo.
    //
    // La blancura que crece hacia la costa NO sale de aqui: sale de `shoreFoam`, que mira el indice de
    // rompiente. Queda una separacion limpia — borreguillos de VIENTO contra agua blanca de ROMPIENTE,
    // cada uno con su oraculo.
    const float W  = glm::clamp(oceanWhitecapCoverage(st), 1.0e-5f, 0.5f);
    // ⚠️ SUELO OBLIGATORIO: con `fade = 0` (fuera del alcance de la ola) todos los trenes salen por el
    // `amp <= 1e-4` y `sigma` queda en CERO, con lo que `smoothstep(0, 0, ...)` divide por cero y
    // devuelve **NaN** — y un NaN en la espuma no se ve como "espuma rara", se lleva el pixel entero.
    // Lo cazo el test del `fade = 0`, que existe justo para esto.
    const float Tm = std::max(sigma * OCEAN_WHITECAP_FIT * std::sqrt(-2.0f * std::log(W)), 1.0e-6f);
    const float capFoam = glm::smoothstep(OCEAN_WHITECAP_LO * Tm, OCEAN_WHITECAP_HI * Tm, slope);
    // ⚠️ Y EL TERCER TERMINO, QUE ORDENA EL PERFIL: la ola que ROMPE. Sin el, la espuma medida salia
    // INVERTIDA — 33 % a 8 m de fondo y 0 % a 1,5 m, o sea que la zona de rompientes era lo MENOS
    // blanco de la costa. La causa: el limite de rompiente aplasta la amplitud justo donde la ola
    // revienta (Σamp cae de 3,89 a 0,825 entre 8 y 1,5 m), asi que la PENDIENTE tambien cae — y un
    // criterio de pendiente, por correcto que sea, no puede ver espuma donde la ola YA se ha roto.
    //
    // El indice de rompiente si lo ve, y crece monotono hacia la orilla (0,97 a 8 m · 3,31 a 3 m ·
    // 7,85 a 1,5 m). Arranca en el 0,78 clasico. Sustituye a la banda por PROFUNDIDAD que habia
    // (`smoothstep(1.6, 0.7, d/2A₀)`): era la misma idea escrita en metros en vez de en fisica — no
    // sabia nada de la ola, solo de cuanta agua hay debajo.
    const float shoreFoam = glm::smoothstep(0.78f, 1.60f, oceanBreakIndex(depthM, st, fetchM));
    return glm::clamp(std::max(capFoam, shoreFoam) * fade, 0.0f, 1.0f);
}

/// Sobrecarga con el mar de referencia. Ver la nota de `oceanWaveHeight`.
inline float oceanFoam(const glm::vec3& wp, const glm::vec3& up, float t,
                       float depthM, float fade = 1.0f) {
    static const OceanState s_ref = oceanDefaultState();
    return oceanFoam(wp, up, t, depthM, fade, s_ref);
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
                             float fetchM = WATER_FETCH_UNLIMITED,
                             const glm::vec3& upSlope = glm::vec3(0.0f)) {
    const glm::vec3 t1 = glm::normalize(std::abs(up.y) < 0.99f ? glm::cross(up, glm::vec3(0, 1, 0))
                                                               : glm::cross(up, glm::vec3(1, 0, 0)));
    const glm::vec3 t2 = glm::cross(up, t1);
    // Bajío y tope de rompiente: el bajío es por tren (cada longitud de onda crece a su ritmo) pero
    // el tope es COMÚN a los cuatro, porque el índice de rompiente habla de la ola entera.
    // El FETCH entra multiplicando el bajío: es cuánta ola puede haber aquí antes de que el fondo la
    // amplifique. Sin él, un lago hereda el swell del océano — ver `oceanFetchFactor`.
    const float green = oceanGreenGain(depthM) * oceanFetchFactor(st, fetchM);
    const float scale = oceanBreakScale(depthM, st, fetchM);
    // ⚠️ EL 2º ARMONICO TAMBIEN AQUI, y no es opcional: esta es la cota que usa la FISICA para flotar
    // y para ahogarse. Si solo estuviera en el shader, lo que se ve y lo que se nada volverian a ser
    // dos superficies distintas — la misma discrepancia que este fichero existe para impedir.
    float h = 0.0f;
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        // ⚠️ GUARDIA de `λ > 0`: un `OceanState` a medio llenar da `k = 2π/0` = infinito y NaN, y el
        // NaN no se ve como "una ola rara" sino como que el agua DESAPARECE. Ya paso con el UBO del
        // banco al subir de 4 a 8 trenes. Gemelo del guardia de `harukaWaveAt`.
        if (!(st.wave[i][0] > 0.0f)) continue;
        // ⚠️ `λ` DE LA TABLA ES LA DE AGUAS PROFUNDAS. La local sale de la dispersión finita, y `ω`
        // —que es lo que se conserva— sigue siendo la profunda. Ver `oceanWaveNumber`.
        const float k0 = 6.2831853f / st.wave[i][0];
        const float k  = oceanWaveNumber(k0, depthM);
        const float amp = st.wave[i][1] * green * scale * fade * oceanShortWaveFade(k);
        if (amp <= 1e-4f) continue;
        // ⚠️ REFRACTADA. La direccion de la tabla es la de AGUAS PROFUNDAS; al perder fondo la ola
        // gira hacia la orilla (`oceanRefract`). Sin pendiente el vector sale identico.
        const glm::vec3 D = oceanRefract(glm::normalize(t1 * st.wave[i][2] + t2 * st.wave[i][3]),
                                         upSlope, up, k0, k);
        const float w  = std::sqrt(OCEAN_G * k0);         // la frecuencia NO cambia con el fondo
        const float ph = k * glm::dot(D, wp) - w * t;
        // Componente a lo largo de `up`, con el 2º armonico POR TREN. No cambia la altura pico-valle,
        // asi que el limite de rompiente y la flotabilidad siguen significando lo mismo.
        h += amp * (std::sin(ph) - oceanStokesSkew(k, amp, depthM) * std::cos(2.0f * ph));
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
                                   float fetchM = WATER_FETCH_UNLIMITED,
                                   const glm::vec3& upSlope = glm::vec3(0.0f)) {
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
    const float steepScale = oceanSteepScale(depthM, st, fetchM);
    const float curl       = oceanCurlGain(depthM, st, fetchM);
    const float breaker    = oceanBreakerGain(depthM, st, fetchM);
    (void)breakiness;
    glm::vec3 v(0.0f);
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        // Mismas tres líneas que `oceanWaveHeight`, y por el mismo motivo: si se separaran, la
        // velocidad dejaría de ser la derivada de la altura y `ocean_wave_velocity` lo cazaría.
        const float k0 = 6.2831853f / st.wave[i][0];
        const float k  = oceanWaveNumber(k0, depthM);
        const float amp = st.wave[i][1] * green * scale * fade * oceanShortWaveFade(k);
        if (amp <= 1e-4f) continue;
        // ⚠️ AQUI FALTABA LA REFRACCION, Y EL PARAMETRO YA LLEGABA. `upSlope` se aceptaba en la firma
        // y no se usaba NUNCA: `oceanWaveHeight` giraba la ola al perder fondo y esto no, asi que en
        // una playa la ALTURA la daba una ola refractada y la VELOCIDAD del agua apuntaba en la
        // direccion del mar abierto. Las dos salen de la misma `disp`; si divergen, la velocidad deja
        // de ser su derivada.
        //
        // No se veia porque el unico test de velocidad la compara con la altura SIN pendiente, donde
        // `oceanRefract` devuelve `d0` exacto y el fallo es invisible. Y el llamante real SI la pasa:
        // `PlanetarySystem::sampleWaterVelocity` calcula `slopeV` y la entrega.
        const glm::vec3 D = oceanRefract(glm::normalize(t1 * st.wave[i][2] + t2 * st.wave[i][3]),
                                         upSlope, up, k0, k);
        const float w  = std::sqrt(OCEAN_G * k0);         // la frecuencia NO cambia con el fondo
        const float ph = k * glm::dot(D, wp) - w * t;
        // Q = escarpado, GEMELO EXACTO de `ocean_wave.glsl`. Entra en la velocidad horizontal porque
        // es quien reparte cuánto del círculo es avance y cuánto subida — y al romper crece, que es
        // por lo que la cresta de una ola que revienta te empuja hacia la playa mucho más que el
        // mismo oleaje en agua honda.
        // ⚠️ EL SEMIEJE HORIZONTAL DE LA ELIPSE, no `Q·A`. Ver `oceanHorizAmp`: el reparto viejo no
        // dependia de la amplitud, asi que la velocidad horizontal de una charca era la del oceano.
        // El ADELANTO DE LA CRESTA (ver `oceanCurlGain`): concentrado en `cos φ > 0`, o sea en la
        // mitad delantera de la ola. Es lo que hace que una rompiente te empuje hacia la playa mucho
        // mas que el mismo oleaje en agua honda.
        const float cph = std::cos(ph);
        const float horiz = oceanHorizAmp(k, amp, depthM) * steepScale
                          * (1.0f + curl * std::max(0.0f, cph))
                          * ((i == 0) ? (1.0f + breaker) : 1.0f);   // ver `oceanBreakerGain`
        // ⚠️ LA VERTICAL LLEVA EL 2º ARMONICO DERIVADO. La elevacion es `amp·(sin φ − S·cos 2φ)` y
        // `∂φ/∂t = −ω`, asi que la velocidad vertical es `−amp·ω·(cos φ + 2S·sin 2φ)`. Sin el segundo
        // termino la velocidad dejaria de ser la derivada de la altura y `ocean_wave_velocity` lo
        // cazaria — que es justo para lo que esta ese test.
        const float skew = oceanStokesSkew(k, amp, depthM);
        v += D * (horiz * w * std::sin(ph))
           - up * (amp * w * (cph + 2.0f * skew * std::sin(2.0f * ph)));
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
