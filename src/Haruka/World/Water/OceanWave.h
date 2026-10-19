/**
 * @file OceanWave.h
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
#include "Haruka/World/Water/WaterFill.h"   // WATER_FETCH_UNLIMITED: el fetch del oceano

namespace Haruka::World::Water { 

inline constexpr float  OCEAN_G     = 9.81f;
inline constexpr int    OCEAN_WAVES = 8;

/**
 * @brief EL reloj del oleaje. Uno solo para todo el motor.
 *
 * ⚠️ EXISTE PORQUE HABÍA DOS. `Planet.cpp` llenaba `uDebug.y` con un `static` suyo y
 * `PlanetarySystem.cpp` evaluaba la ola de la física con OTRO `static`, cada uno inicializado en su
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
    /// El viento que LEVANTÓ este mar (m/s), ya filtrado en el tiempo. 0 = no se sabe (estado por
    /// defecto): entonces se deduce de Hs. Ver `oceanWindSpeed`.
    float windMS;
    /**
     * DESFASE por tren (rad), sumado a la fase `k·(D·x) − ω·t`.
     *
     * ⚠️ `x` ES LA POSICIÓN RELATIVA AL ANCLA DEL MAR, NO AL CENTRO DEL PLANETA. Con `x = dir·R` y
     * `D` en el plano tangente de `dir`, `D·x` era CERO en todo el mar: la fase valía `−ω·t` en todas
     * partes, el océano entero subía y bajaba EN BLOQUE y "las olas" eran el ruido de float de un
     * producto escalar a 6,4e6 m (medido: σ espacial de la cota 1 cm en el marco del juego contra
     * 62 cm en marco plano). Ese era "no veo olas / el agua palpita". La fase se mide desde un ancla
     * fija del mundo cerca de la cámara, y al moverla se compensa aquí para que ninguna cresta salte.
     */
    float phase[OCEAN_WAVES];
    /**
     * La parte de cada tren que es MAR DE FONDO (m de amplitud, incluida en `wave[i][1]`). La pone
     * `oceanAddSwell`; 0 = todo es mar de viento. Existe para que el agua CERRADA no la reciba: el
     * swell viene de un temporal a miles de km y no cabe en un lago (ver `oceanAmp`).
     */
    float swell[OCEAN_WAVES];
};

/// El estado por defecto: la tabla de referencia y el mar a su cota. Es lo que se ve si nadie
/// publica un estado — de modo que no configurar nada da exactamente el mar de antes.
inline OceanState oceanDefaultState() {
    OceanState s{};
    for (int i = 0; i < OCEAN_WAVES; ++i)
        for (int c = 0; c < 4; ++c) s.wave[i][c] = OCEAN_WAVE[i][c];
    s.seaLevelM = 0.0f;
    s.windMS = 0.0f;
    for (int i = 0; i < OCEAN_WAVES; ++i) { s.phase[i] = 0.0f; s.swell[i] = 0.0f; }
    return s;
}

/// Viento mínimo y máximo que el mar traduce a oleaje (m/s). Por debajo del mínimo el mar es un
/// rizo de centímetros (la mar de fondo de otro sitio no se modela); por encima del máximo la ola
/// no cabe en la banda de 8,7-137 m ni en la costa. ⚠️ El mínimo era 4 m/s ("nunca un espejo"): con
/// el viento del clima en 3-5 m/s eso hacía el mar INDEPENDIENTE del clima la mitad del tiempo.
inline constexpr float OCEAN_WIND_MIN = 1.5f;
inline constexpr float OCEAN_WIND_MAX = 22.0f;
/// Constantes de tiempo con las que el MAR sigue al VIENTO (s): crece en ~20 min, amaina en ~1 h
/// (la mar de fondo sobrevive al viento que la hizo). Sin esto el estado del mar se rehacía cada
/// frame con el viento instantáneo del clima, que en un punto fijo salta hasta 7,8 m/s en UN segundo
/// (medido): la Hs de todo el océano saltaba 5,7 m de un frame al siguiente.
inline constexpr float OCEAN_WIND_GROW_S  = 1200.0f;
inline constexpr float OCEAN_WIND_DECAY_S = 3600.0f;

/// LA MAR DE FONDO de una costa que recibe la de una borrasca moderada (m, trenes LARGOS:
/// `λ >= OCEAN_SWELL_MIN_LAMBDA`). Es la que usan los TESTS de costa; el JUEGO pasa la que dice el
/// clima (`oceanSwellFromWeather`). Ver `oceanAddSwell`.
inline constexpr float OCEAN_SWELL_AMP        = 0.25f;
inline constexpr float OCEAN_SWELL_MIN_LAMBDA = 80.0f;

/// LA MARJADILLA MINIMA: escarpado minimo `k·a` de los trenes medios y cortos (λ < 80 m). 0,06 hasta
/// 19 m, bajando en log hasta 0,02 a 61 m. Lo que el ojo lee como "ola" es la PENDIENTE: el swell
/// de 137 m con 25 cm tiene un 1 % y se ve como un plano (`coast_open_sea_waves_visible`).
inline float oceanMinSteepness(float lambdaM) {
    if (lambdaM >= OCEAN_SWELL_MIN_LAMBDA) return 0.0f;
    if (lambdaM <= 19.0f) return 0.06f;
    const float u = std::log(lambdaM / 19.0f) / std::log(61.0f / 19.0f);
    return std::max(0.06f - 0.04f * u, 0.02f);
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
 * Las LONGITUDES de los ocho trenes y los ángulos entre ellos (son las que dan el aspecto ya
 * afinado, y una λ que no cambia es una fase que no se revuelve — ver el cuerpo). El viento mueve la
 * AMPLITUD de cada tren (PM evaluado en su frecuencia) y el rumbo. A `U = OCEAN_REF_WIND` esto
 * reproduce la tabla original exactamente.
 *
 * ⚠️ QUIÉN LLAMA A ESTO NO LE PASA EL VIENTO INSTANTÁNEO DEL CLIMA: le pasa el viento FILTRADO
 * (`OCEAN_WIND_GROW_S` / `OCEAN_WIND_DECAY_S`). El mar es una integral del viento, no su valor.
 *
 * @param windSpeed     módulo del viento (m/s).
 * @param windDirX/Y    rumbo del viento en el plano tangente (no hace falta normalizarlo).
 * @param seaLevelM     cota de la lámina (marea incluida), en metros.
 */
inline OceanState oceanStateFromWind(float windSpeed, float windDirX, float windDirY,
                                     float seaLevelM = 0.0f) {
    OceanState s = oceanDefaultState();
    s.seaLevelM = seaLevelM;

    const float U = std::clamp(windSpeed, OCEAN_WIND_MIN, OCEAN_WIND_MAX);
    s.windMS = U;

    // ── λ FIJA POR TREN; LA AMPLITUD SIGUE A PIERSON-MOSKOWITZ EN ESA FRECUENCIA ────────────────
    //
    // ⚠️ ANTES λ ESCALABA CON U² Y ESO REVOLVÍA LA FASE DEL MAR ENTERO. La fase es `k·(D·x) − ω·t`:
    // un cambio de `k` cambia la fase en `dk·x`, y a 10 km del ancla un metro de λ en el tren de
    // 137 m son 3 rad — cada variación del viento hacía "hervir" la superficie sin que ninguna ola
    // viajara. Con la banda de frecuencias FIJA sólo cambian amplitudes y rumbo, y una amplitud
    // que cambia despacio (ver `OCEAN_WIND_GROW_S`) no mueve ninguna cresta de sitio.
    //
    // La amplitud de cada tren es la de la tabla reescalada por el cociente de espectros PM en SU
    // frecuencia: `S(ω;U)/S(ω;Uref) = exp(−1,25·((ωp(U)/ω)⁴ − (ωp_ref/ω)⁴))`, con `ωp ∝ 1/U`. A
    // `U = Uref` es 1,0 exacto (la tabla de referencia). Con viento flojo los trenes largos mueren
    // (a 4 m/s el de 137 m vale 0 y el de 8,7 m el 72 %); con temporal los largos crecen hasta la
    // cola `ω⁻⁵`, que NO depende del viento — y por eso la Hs de un temporal sale BAND-LIMITED (a
    // 22 m/s ~5,6 m contra los 10,4 de PM entero: la energía de λ > 137 m no está en la banda). Es
    // una limitación conocida y preferible a olas de 137 m con 3 m de amplitud.
    const float wpRef = std::sqrt(OCEAN_G * 6.2831853f / OCEAN_WAVE[0][0]);   // pico PM de la tabla
    const float wpU   = wpRef * (OCEAN_REF_WIND / U);
    const float wl = std::sqrt(windDirX * windDirX + windDirY * windDirY);
    const float cx = (wl > 1e-6f) ? windDirX / wl : 1.0f;
    const float cy = (wl > 1e-6f) ? windDirY / wl : 0.0f;
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        const float lam = OCEAN_WAVE[i][0];
        const float w   = std::sqrt(OCEAN_G * 6.2831853f / lam);
        const float rU  = wpU / w, rR = wpRef / w;
        const float ratio = std::exp(-1.25f * (rU * rU * rU * rU - rR * rR * rR * rR));
        s.wave[i][0] = lam;
        s.wave[i][1] = OCEAN_WAVE[i][1] * std::sqrt(ratio);
        // Rotación 2D del vector de dirección por el rumbo del viento.
        const float dx = OCEAN_WAVE[i][2], dy = OCEAN_WAVE[i][3];
        s.wave[i][2] = dx * cx - dy * cy;
        s.wave[i][3] = dx * cy + dy * cx;
    }
    return s;
}

/**
 * @brief LA MAR DE FONDO QUE DICE EL CLIMA: la que llega aquí de los vientos que sopla el clima lejos.
 *
 * ⚠️ ANTES ERA UN SUELO FIJO (0,25 m en los trenes largos SIEMPRE, y un escarpado mínimo en los medios):
 * con 3,5 m/s de viento el mar salía con ~1,6 m de Hs donde Pierson-Moskowitz da 0,27, y en la orilla
 * (1-6 m de fondo) esa ola se plegaba en láminas como cristal roto (captura del 18-10). Andoni: "el
 * clima decide". Ahora: cada muestra es un viento del clima `U` a una distancia `d`; su mar de fondo
 * es la amplitud del tren LARGO de un mar plenamente desarrollado a ese viento (la de
 * `oceanStateFromWind`) atenuada con la distancia (`e^(−d/OCEAN_SWELL_DECAY_M)`: dispersión y
 * apertura del frente de ondas). Se queda la MAYOR. Sin viento fuerte en ningún sitio, no hay.
 */
inline constexpr float OCEAN_SWELL_DECAY_M = 3.0e6f;
inline float oceanSwellFromWeather(const float* windMS, const float* distM, int n) {
    float amp = 0.0f;
    for (int i = 0; i < n; ++i) {
        float a0 = 0.0f;
        const OceanState far = oceanStateFromWind(windMS[i], 1.0f, 0.0f);
        for (int j = 0; j < OCEAN_WAVES; ++j)
            if (far.wave[j][0] >= OCEAN_SWELL_MIN_LAMBDA) a0 = std::max(a0, far.wave[j][1]);
        amp = std::max(amp, a0 * std::exp(-std::max(distM[i], 0.0f) / OCEAN_SWELL_DECAY_M));
    }
    return amp;
}

/**
 * @brief LA MAR DE FONDO sobre un mar de viento: el estado que el JUEGO usa.
 *
 * ⚠️ SIN ESTO LA COSTA ESTABA QUIETA CON EL CLIMA DE CASI SIEMPRE. Pierson-Moskowitz a viento flojo
 * mata justo los trenes largos (a 3,5 m/s el de 137 m vale ~0), y son los que dan la ROMPIENTE y la
 * trepada: en la partida del 16-10, viento 3,5 m/s -> Hs 0,27 m, orilla inmovil y ni una cresta
 * plegada (`coast_surf_*`). Una costa real recibe oleaje de temporales lejanos aunque aqui no sople:
 * el swell viaja miles de km.
 *
 * Se modela como un SUELO de amplitud en los trenes largos (Hs ~1 m entre los dos) y de ESCARPADO
 * en los medios (`oceanMinSteepness`, Hs ~0,6 m más): sin lo segundo el swell de 137 m llega a la
 * costa pero el mar abierto se ve como un plano. No es un tren aparte: la banda de λ y las fases no
 * cambian. Con viento fuerte PM ya los supera y no hace nada.
 * La parte añadida queda anotada en `swell[i]`, y el agua de fetch limitado no la recibe (`oceanAmp`).
 * `oceanStateFromWind` sigue siendo el mar de viento puro (lo que miden `ocean_sea_state` y
 * `ocean_low_wind_spectrum`).
 */
inline OceanState oceanAddSwell(OceanState s, float swellAmp = OCEAN_SWELL_AMP) {
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        const float wind = s.wave[i][1] - s.swell[i];
        // Largos: amplitud minima (el swell). Medios y cortos: escarpado minimo (la marejadilla que
        // hace que el mar abierto se vea ondulado aunque no sople aqui).
        const float lam  = s.wave[i][0];
        // ⚠️ El escarpado mínimo de los medios (`oceanMinSteepness`) ya no se aplica: era un mar
        // inventado que no venía del clima. Solo los trenes largos reciben la mar de fondo.
        const float floorAmp = (lam >= OCEAN_SWELL_MIN_LAMBDA) ? swellAmp : 0.0f;
        const float tot  = std::max(wind, floorAmp);
        s.wave[i][1] = tot;
        s.swell[i]   = tot - wind;
    }
    return s;
}

/**
 * @brief EL MAR SIGUE AL VIENTO — y lleva el ancla de la fase. Pieza pura (sin GPU ni planeta) para
 *        poder medirla en `test_ocean_sea_follows_wind`, igual que `CloudDrift` con las nubes.
 *
 * Dos cosas que vivían sueltas en `PlanetarySystem::updateOceanState` y que por eso nadie medía:
 *
 *  1. EL VIENTO FILTRADO. El estado del mar se rehacía cada frame con el viento INSTANTÁNEO del
 *     clima, que en un punto fijo salta hasta 7,8 m/s en un segundo (medido, media 0,5 m/s cada
 *     segundo): la Hs de todo el océano saltaba 5,7 m de un frame al siguiente. El mar es una
 *     integral del viento: crece en ~20 min y amaina en ~1 h (`OCEAN_WIND_GROW_S/DECAY_S`), en
 *     segundos del reloj del oleaje (`oceanClockSeconds`, tiempo real), que es el que ve el ojo.
 *
 *  2. EL ANCLA DE LA FASE. La fase `k·(D·x) − ω·t` con `x` planetocéntrica era CERO en todo el mar
 *     (ver `OceanState::phase`). Se mide desde un punto fijo del mundo cerca de la cámara. En una
 *     esfera no hay una fase global barata con `k` local y refracción, así que el ancla se mueve
 *     lo MENOS posible: cada `kReanchorM` (20 km) de camino, compensando `phase[i]` con la `k` de
 *     aguas profundas y el rumbo en el ancla nueva. Residuos, medidos en el test:
 *       · aguas profundas: `k·|ΔA|·|x−B|/R`, 1,9 rad a 13 km del ancla nueva para el tren de 137 m
 *         y < 0,15 rad a 1 km — la curvatura de la esfera entre los dos marcos;
 *       · aguas someras: `(k(d)−k0)·(D·ΔA)`, grande — la rompiente cambia de fase al re-anclar.
 *     Es un salto cada 20 km de viaje, no un movimiento continuo. ⚠️ Y un cambio de RUMBO del viento
 *     gira el campo alrededor del ancla: a `r` del ancla las crestas barren a `k·r·dθ/dt`. Con el
 *     filtro (τ ≥ 20 min) y r ≤ 20 km son ≤ 0,4 rad/s en el borde y 0,02 a 1 km. Trenes con rumbo
 *     FIJO y amplitud por reparto direccional lo quitarían del todo; no está hecho.
 */
struct SeaFollower {
    glm::vec2  windSea{0.0f};      ///< viento filtrado (las dos componentes tangentes que da el clima)
    float      lastT   = -1.0f;    ///< reloj del oleaje en el último `advance`
    bool       anchored = false;
    glm::dvec3 anchor{0.0};        ///< el ANCLA (mundo): desde aquí se mide la fase
    float      phase[OCEAN_WAVES] = {};
    int        reanchors = 0;      ///< cuántas veces se ha movido el ancla (para medirlo)
    static constexpr double kReanchorM = 20000.0;
    static constexpr float  kMaxStepS  = 5.0f;   ///< un salto del reloj no es viento

    /// El viento filtrado tras este paso. `windNow`: viento instantáneo; `nowS`: reloj del oleaje.
    const glm::vec2& filterWind(const glm::vec2& windNow, float nowS) {
        if (lastT < 0.0f) { windSea = windNow; lastT = nowS; return windSea; }
        const float dt = std::clamp(nowS - lastT, 0.0f, kMaxStepS);
        lastT = nowS;
        const float tau = (glm::length(windNow) > glm::length(windSea)) ? OCEAN_WIND_GROW_S
                                                                        : OCEAN_WIND_DECAY_S;
        windSea += (windNow - windSea) * (1.0f - std::exp(-dt / tau));
        return windSea;
    }

    /// Mueve el ancla si hace falta (cámara a más de `kReanchorM`) y compensa las fases de `st`.
    /// `st` ya lleva los rumbos de este frame. Devuelve true si el ancla se movió.
    bool anchorFor(OceanState& st, const glm::dvec3& camPos, const glm::dvec3& planetC, double R) {
        const glm::dvec3 rel = camPos - planetC;
        const double len = glm::length(rel);
        if (len < 1e-9) { for (int i = 0; i < OCEAN_WAVES; ++i) st.phase[i] = phase[i]; return false; }
        bool moved = false;
        if (!anchored || glm::length(camPos - anchor) > kReanchorM) {
            const glm::dvec3 up  = rel / len;
            const glm::dvec3 nue = planetC + up * R;
            if (anchored) {
                // Marco tangente en el ancla NUEVA, con la misma construcción que las funciones de
                // ola (`t1 = cross(up, +Y)`), y la `k` de aguas profundas: en el ancla nueva la
                // fase queda EXACTAMENTE la que tenía. Lo que se aleja paga la curvatura.
                const glm::vec3 upf(up);
                const glm::vec3 t1 = glm::normalize(std::abs(upf.y) < 0.99f ? glm::cross(upf, glm::vec3(0, 1, 0))
                                                                            : glm::cross(upf, glm::vec3(1, 0, 0)));
                const glm::vec3 t2 = glm::cross(upf, t1);
                // Antes: `ph = k·D·(x − A) + φ`; después: `k·D·(x − B) + φ'`. Iguales en `x = B` si
                // `φ' = φ + k·D·(B − A)`.
                const glm::dvec3 dA = nue - anchor;   // ancla nueva − vieja
                for (int i = 0; i < OCEAN_WAVES; ++i) {
                    if (!(st.wave[i][0] > 0.0f)) continue;
                    const glm::vec3 D = glm::normalize(t1 * st.wave[i][2] + t2 * st.wave[i][3]);
                    const double k0 = 6.2831853 / (double)st.wave[i][0];
                    double ph = (double)phase[i] + k0 * glm::dot(glm::dvec3(D), dA);
                    ph -= 6.28318530717958647692 * std::floor(ph / 6.28318530717958647692 + 0.5);
                    phase[i] = (float)ph;
                }
                ++reanchors;
                moved = true;
            }
            anchor = nue; anchored = true;
        }
        for (int i = 0; i < OCEAN_WAVES; ++i) st.phase[i] = phase[i];
        return moved;
    }
};

/**
 * @brief MAREA: cota de la lámina (m) en una dirección, por el término de marea de Legendre P₂.
 *
 * `h = Σ GM·(3cos²θ − 1) / (2·D³) · escala`, que es el potencial de marea de un cuerpo puntual: dos
 * abultamientos opuestos (bajo la luna y en las antípodas), que es la razón de que haya DOS pleamares
 * al día y no una. `floorD` evita la singularidad si un cuerpo pasa por el centro.
 *
 * Gemelo del cálculo que ya vivía en `ApplicationRender.cpp` para el fluido — se trae aquí para que
 * la marea que ve la física, la que dibuja el mar y la que pinza los ríos sean UNA.
 */
inline double oceanTideAt(const glm::dvec3& dir, double planetRadius,
                          const glm::dvec3* bodyPos, const double* bodyGM, int bodyCount) {
    if (!bodyPos || !bodyGM || bodyCount <= 0 || planetRadius <= 0.0) return 0.0;
    // ⚠️ EXAGERACION x3, NO x15. El x15 venia del fluido y no hacia daño porque al mar no le llegaban
    // cuerpos (marea 0,00 m en el log). Con el Sol y la Luna REALES (`SkyEphemeris.h`) daba 12 m de
    // rango en marea viva: las playas enteras se inundaban dos veces al dia. x3 da ~2,4 m en viva y
    // ~0,8 m en muerta (`sky_tide`), una costa normal: se ve subir y bajar sin tragarse la playa.
    const double scale  = (planetRadius * planetRadius / 9.8) * 3.0;
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
    // Si el estado trae el viento que lo levantó, ése es. Con λ fijas la Hs es band-limited y ya
    // no invierte PM (a 22 m/s daría 16): la deducción queda para el estado por defecto.
    if (st.windMS > 0.0f) return st.windMS;
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

/// ¿Agua CERRADA (lago, charco), con su fetch? `<= 0` es "este punto no trae fetch": océano.
inline bool oceanFetchLimited(float fetchM) {
    return fetchM > 0.0f && fetchM < WATER_FETCH_UNLIMITED * 0.5f;
}

/// Amplitud del tren `i` para el agua con este fetch: en mar abierto, entera; en agua cerrada, SIN la
/// mar de fondo (`OceanState::swell`). Gemelo de `harukaAmpAt`.
inline float oceanAmp(const OceanState& st, int i, float fetchM) {
    return oceanFetchLimited(fetchM) ? st.wave[i][1] - st.swell[i] : st.wave[i][1];
}

inline float oceanFetchFactor(const OceanState& st, float fetchM) {
    // Océano, o sin dato: no se limita nada. `<= 0` es "este punto no trae fetch", no "fetch cero".
    if (!oceanFetchLimited(fetchM)) return 1.0f;
    // Contra el MAR DE VIENTO: el fetch limita lo que el viento levanta aqui; el swell ni entra.
    float sum2 = 0.0f;
    for (int i = 0; i < OCEAN_WAVES; ++i) { const float a = oceanAmp(st, i, fetchM); sum2 += a * a; }
    const float Hs = 4.0f * std::sqrt(sum2 * 0.5f);
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
                             float fetchM = WATER_FETCH_UNLIMITED,
                             float quadM = OCEAN_MIN_QUAD_M) {
    const float green = oceanGreenGain(depthM) * oceanFetchFactor(st, fetchM);
    float sum = 0.0f;
    // ⚠️ CON EL DESVANECIDO DE ONDA CORTA DENTRO. El tope habla de la altura que de verdad hay; si
    // sumara trenes que la dispersion finita ya ha apagado, acotaria una ola que no existe y dejaria
    // pasar mas de la que si.
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        const float k0 = 6.2831853f / st.wave[i][0];
        sum += oceanAmp(st, i, fetchM) * oceanShortWaveFade(oceanWaveNumber(k0, depthM), quadM);
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
                             float fetchM = WATER_FETCH_UNLIMITED,
                             float quadM = OCEAN_MIN_QUAD_M) {
    return glm::clamp(1.0f - oceanBreakScale(depthM, st, fetchM, quadM), 0.0f, 1.0f);
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
                             float fetchM = WATER_FETCH_UNLIMITED,
                             float quadM = OCEAN_MIN_QUAD_M) {
    if (!(depthM > 1.0e-4f)) return 0.0f;
    const float green = oceanGreenGain(depthM) * oceanFetchFactor(st, fetchM);
    float sum = 0.0f;
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        const float k0 = 6.2831853f / st.wave[i][0];
        sum += oceanAmp(st, i, fetchM) * oceanShortWaveFade(oceanWaveNumber(k0, depthM), quadM);
    }
    return 2.0f * sum * green / depthM;     // H = 2·Σamp (pico a pico)
}

/**
 * @brief El fondo más hondo en el que esta mar ya ROMPE (`H/d ≥ 0,78`), en metros; 0 = no rompe en
 *        ningún fondo de más de 2 cm. Es el borde exterior de la franja de rompiente: quien tenga que
 *        dibujar la ola rota (la malla fina del agua) sabe con esto qué suelo la lleva.
 *
 * Barrido de fuera hacia dentro a paso de 5 cm: el índice crece al bajar el fondo (Green `d^-1/4`
 * contra `1/d`), así que el primer fondo que pasa es el borde. Coste ~600 evaluaciones, una vez por
 * cambio de mar, no por frame.
 */
inline float oceanBreakDepth(const OceanState& st, float maxDepthM = 30.0f) {
    for (float d = maxDepthM; d >= 0.02f; d -= 0.05f)
        if (oceanBreakIndex(d, st) >= 0.78f) return d;
    return 0.0f;
}

/// La CARA de la ola al romper (m, pico a pico): `H = 2·Σamp·Green` sin recortar, en el fondo donde
/// rompe. Lo que la malla tiene que poder dibujar (`coast_surf_breaks`). 0 = no rompe.
inline float oceanBreakerFaceM(const OceanState& st) {
    const float db = oceanBreakDepth(st);
    if (db <= 0.0f) return 0.0f;
    float sum = 0.0f;
    for (int i = 0; i < OCEAN_WAVES; ++i) sum += st.wave[i][1];
    return 2.0f * sum * oceanGreenGain(db);
}

inline float oceanCurlGain(float depthM, const OceanState& st,
                           float fetchM = WATER_FETCH_UNLIMITED,
                             float quadM = OCEAN_MIN_QUAD_M) {
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
    const float gamma = oceanBreakIndex(depthM, st, fetchM, quadM);
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
                              float fetchM = WATER_FETCH_UNLIMITED,
                             float quadM = OCEAN_MIN_QUAD_M) {
    // Arranca en el 0,78 clásico —el índice al que una ola revienta— y no en el 0,60 del adelanto de
    // cresta: peraltarse es una cosa y volcar es otra, y no deben empezar juntas.
    const float gamma = oceanBreakIndex(depthM, st, fetchM, quadM);
    return OCEAN_BREAKER_MAX * glm::smoothstep(0.78f, 1.20f, gamma);
}

/**
 * @brief EL LABIO: la cresta que se lanza por delante de su cara y cae — el "rollo de papel".
 *        Gemelo de `harukaLipGain` / el bloque del labio de `harukaGerstner`.
 *
 * ⚠️ NO SALE DE UN TREN, SALE DE LA OLA ENTERA. Montado sobre el tren 0 (como `breaker`) no forma
 * tubo: con la mar del juego ese tren mide 0,42 m dentro de una ola de 3,6 m que suman los ocho, y su
 * cresta no es la que se ve. Aquí el labio lee la superficie COMPUESTA:
 *
 *     e = η / Σamp            (0..1: cuánto de cresta hay aquí; η = Σ amp·sin φ, sin el 2º armónico)
 *     u = e^P                 (solo cerca de la cresta)
 *     f = clamp(−∂η/∂Dl / Σamp·k, 0, 1)   (solo en la cara DELANTERA, la que mira a la orilla)
 *     labio = Dl·(L·u) − up·(L·caída·u·f),   L = ganancia·Σamp
 *
 * con `Dl` el rumbo REFRACTADO del tren 0 (el que rompe: hacia la orilla). La cresta avanza más que
 * su cara (`u` cae al bajar), así que la adelanta; y lo que queda delante baja (`f`): un labio que
 * cuelga sobre un hueco de aire. Proporcional a `Σamp`, y cerrado por el índice de rompiente — las
 * dos puertas del agua interior (ver `oceanBreakerGain`).
 *
 * La ventana: nace en el 0,78 clásico (revienta), está entero hacia 1,0 y se deshace entre 1,3 y 1,7,
 * donde la ola ya es espuma (`shoreFoam` sube de 0,78 a 1,60). Las tres cifras de forma (2,5 · 4 · 1)
 * salen de un barrido medido con la mar del juego (`coast_lip_barrel`): con menos no queda hueco bajo
 * el labio, con más el labio sale más largo que la propia ola. La caída era 2 y bajó a 1 (18-10): con 2
 * la punta bajaba hasta la cara y la atravesaba (`coast_lip_no_loop`); con 1 cae por delante y el hueco
 * sigue (~1 m en los tres mares).
 */
inline constexpr float OCEAN_LIP_MAX = 2.5f;
inline constexpr float OCEAN_LIP_POW = 4.0f;
inline constexpr float OCEAN_LIP_DROOP = 1.0f;

inline float oceanLipGain(float depthM, const OceanState& st,
                          float fetchM = WATER_FETCH_UNLIMITED,
                          float quadM = OCEAN_MIN_QUAD_M) {
    const float gamma = oceanBreakIndex(depthM, st, fetchM, quadM);
    return OCEAN_LIP_MAX * glm::smoothstep(0.78f, 1.0f, gamma) * (1.0f - glm::smoothstep(1.3f, 1.7f, gamma));
}

/// Lo que el labio necesita de la ola compuesta, acumulado en el MISMO bucle de los trenes (sin otra
/// pasada). Gemelo de las variables `lip*` de `harukaGerstner`.
struct OceanLipAcc {
    glm::vec3 Dl{0.0f};        ///< rumbo del tren 0 refractado (0 = el tren 0 no esta: sin labio)
    float     eta = 0.0f;      ///< Σ amp·sin φ
    float     sumA = 0.0f;     ///< Σ amp
    float     sumAK = 0.0f;    ///< Σ amp·k
    glm::vec3 grad{0.0f};      ///< ∇η = Σ amp·k·cos φ·D
    glm::vec3 hessL{0.0f};     ///< Σ amp·k²·sin φ·(D·Dl)·D  (−∂(∇η·Dl)/∂T = hessL·T)
    void add(int i, const glm::vec3& D, float amp, float k, float c, float s) {
        if (i == 0) Dl = D;
        eta += amp * s; sumA += amp; sumAK += amp * k;
        grad  += D * (amp * k * c);
        hessL += D * (amp * k * k * s * glm::dot(D, Dl));
    }
    /// Cuánto de cresta (`u`) y de cara delantera (`f`). Sin labio, los dos a 0.
    void shape(float& u, float& f) const {
        u = f = 0.0f;
        if (!(sumA > 0.0f) || !(sumAK > 0.0f) || glm::dot(Dl, Dl) < 0.5f) return;
        u = std::pow(std::max(eta / sumA, 0.0f), OCEAN_LIP_POW);
        f = glm::clamp(-glm::dot(grad, Dl) / sumAK, 0.0f, 1.0f);
    }
    /// Desplazamiento del labio (m). `gain` = `oceanLipGain`.
    glm::vec3 disp(const glm::vec3& up, float gain) const {
        float u, f; shape(u, f);
        const float L = gain * sumA;
        return Dl * (L * u) - up * (L * OCEAN_LIP_DROOP * u * f);
    }
    /// Lo que el labio suma a la divergencia del mapa horizontal (`jac`): ∂(L·u)/∂Dl.
    float jacTerm(float gain) const {
        if (!(sumA > 0.0f) || glm::dot(Dl, Dl) < 0.5f) return 0.0f;
        const float e = eta / sumA;
        if (e <= 0.0f) return 0.0f;
        return gain * OCEAN_LIP_POW * std::pow(e, OCEAN_LIP_POW - 1.0f) * glm::dot(grad, Dl);
    }
};

/**
 * @brief LA FASE DE UN TREN, CONTINUA EN EL ESPACIO (18-10). Gemelo de `harukaTrainPhase`.
 *
 * ⚠️ LA FASE ERA `k·dot(D, wp)` CON EL `k` LOCAL, y eso es ruido en cuanto el fondo varía. `wp` se
 * mide desde el ANCLA del mar (hasta `SeaFollower::kReanchorM`, 20 km), así que dos vértices vecinos
 * con unos centímetros de fondo distinto —una llanura de marea a ras del mar— tienen `k` distintos y
 * fases que difieren en `Δk·|wp|`: decenas de radianes. Cada vértice subía o bajaba la ola al azar y
 * la orilla salía en "tiendas" de triángulos con el borde recto de la malla (Andoni: "se ven como olas
 * pero por low-poly no se ve claro, son vértices"). Medido en el juego: con la fase de aguas
 * profundas las tiendas desaparecen enteras.
 *
 * La fase de verdad es la integral de `k` a lo largo del rayo, y no se puede hacer por punto. Lo que
 * sí se puede: la fase de aguas profundas (`k0·D0·wp`, exacta y continua: la misma que compensa el
 * reanclaje) más el RETRASO que acumula la ola al perder fondo,
 *
 *     Λ(h) = ∫_h^∞ (k − k0) dh' / σ = (1 − √(k0·h))² / σ      (k somera = √(k0/h), k0·h < 1)
 *
 * con `h` el fondo SUAVE (`OCEAN_PHASE_BED_M`, sin el rizo fino: con él la fase volvía a ser ruido) y
 * `σ` una pendiente de playa de referencia. En una playa de pendiente σ el gradiente de `Λ` es
 * exactamente `k − k0` hacia la orilla, o sea la ola se acorta como debe. En otra pendiente se acorta
 * de más o de menos — es la aproximación que se acepta por tener una fase sin ruido.
 *
 * `bedM` = cota del agua EN REPOSO menos el fondo suave (m; negativo = seco). `OCEAN_NO_BED` = el
 * llamante no lo sabe, y queda la ley de siempre (fondo uniforme, donde `k·D·wp` es exacta).
 * ⚠️ LAS DERIVADAS SIGUEN CON `k·D` LOCAL: en la playa de referencia coinciden con el gradiente de
 * esta fase; fuera de ella son una aproximación (la normal y el jacobiano no se recalculan con `Λ`).
 */
inline constexpr float OCEAN_NO_BED       = -1.0e30f;
inline constexpr float OCEAN_PHASE_BED_M  = 64.0f;   ///< corte del fondo que lleva la fase (m)
inline constexpr float OCEAN_PHASE_SLOPE  = 0.02f;   ///< pendiente de playa de referencia (σ)

inline float oceanShoalLag(float k0, float bedM) {
    const float x = std::sqrt(k0 * std::max(bedM, 0.0f));
    return (x < 1.0f) ? (1.0f - x) * (1.0f - x) / OCEAN_PHASE_SLOPE : 0.0f;
}

inline float oceanTrainPhase(const OceanState& st, int i, float k0, float k, const glm::vec3& D,
                             const glm::vec3& wp, const glm::vec3& t1, const glm::vec3& t2,
                             float t, float bedM) {
    const float w = std::sqrt(OCEAN_G * k0);   // la frecuencia NO cambia con el fondo
    if (!(bedM > OCEAN_NO_BED)) return k * glm::dot(D, wp) - w * t + st.phase[i];
    const glm::vec3 D0 = glm::normalize(t1 * st.wave[i][2] + t2 * st.wave[i][3]);
    return k0 * glm::dot(D0, wp) + oceanShoalLag(k0, bedM) - w * t + st.phase[i];
}

/**
 * @brief LA PUERTA DEL ADELANTO: cuánto es este punto CARA DELANTERA de la ola COMPUESTA (0..1), la
 *        misma `f` que usa el labio (`OceanLipAcc::shape`), y su gradiente en el plano tangente.
 *        Gemelo de la pasada previa de `harukaGerstner`.
 *
 * ⚠️ EL ADELANTO IBA POR TREN, CON LA FASE DE CADA UNO (`max(0, −cos φ)`), y la cara delantera de un
 * tren no es la de la ola que se ve: con solo mar de fondo (dos trenes largos parecidos, 18-10) la
 * cara delantera del de 89 m caía sobre la ESPALDA de la cresta compuesta y la plegaba
 * (`coast_lip_front_steep`: espalda 2,74 contra < 0,5; sin adelanto, 0,32). Con la puerta, cada tren
 * sigue empujando su mitad delantera, pero solo donde la ola compuesta baja hacia la orilla.
 * Cuesta una pasada más por los trenes (fase y coseno, sin el resto).
 */
/**
 * @brief TOPE DEL ADELANTO POR TREN (18-10): lo que el adelanto mueve la cara (`hBase·curl`) no pasa de
 *        `OCEAN_CURL_REACH` veces la amplitud VERTICAL del tren. Gemelo de `harukaCurlCap`.
 *
 * ⚠️ En el bajío la elipse orbital se ensancha (`1/tanh(k·d)`) y `hBase` llega a varias veces `amp`.
 * Multiplicado por el adelanto, con solo mar de fondo (viento mínimo) la cara delantera se iba metros
 * hacia ATRÁS, por detrás de la propia espalda de la cresta: el perfil se cruzaba consigo mismo en un
 * lazo enorme (`coast_lip_no_loop`: 240 de 390 cortes aun sin labio). Una cara que vuelca se mide en
 * alturas de ola, no en anchos de órbita. Constante por tren y fondo: no rompe la derivada espacial.
 */
inline constexpr float OCEAN_CURL_REACH = 1.75f;
inline float oceanCurlCap(float curl, float hBase, float amp) {
    return (hBase > 0.0f) ? std::min(curl, OCEAN_CURL_REACH * amp / hBase) : curl;
}

/// Fraccion de cara delantera (`fr`) a la que la puerta ya esta entera. Se eligio JUNTO con
/// `OCEAN_CURL_REACH` y `OCEAN_LIP_DROOP` en un barrido (18-10) de cuatro tests a la vez:
/// `coast_lip_no_loop` (lazos en los 3 mares), `coast_lip_*`, `ocean_break_fold` (jac a 6 m) y
/// `ocean_mesh_stretch`. Con 0,35 / 1,75 / 1,0: lazos 0·0·0, tubo en los tres mares, jac −0,21, 2,29x.
/// Sin el tope del adelanto, 0,35 pliega de sobra pero estira 3,08x y deja lazos en todas partes.
inline constexpr float OCEAN_CURL_GATE_FR = 0.35f;
struct OceanCurlGate {
    float     G = 0.0f;           ///< puerta [0,1]
    glm::vec3 gradG{0.0f};        ///< ∂G/∂x en el plano tangente (0 donde G está topada)
};
inline OceanCurlGate oceanCurlGate(const glm::vec3& wp, const glm::vec3& up,
                                   const glm::vec3& t1, const glm::vec3& t2, float t,
                                   float depthM, float fade, const OceanState& st, float fetchM,
                                   const glm::vec3& upSlope, float quadM, float green, float scale,
                                   float bedM) {
    OceanCurlGate g;
    glm::vec3 Dl(0.0f), hess(0.0f);
    float gradDl = 0.0f, sumAK = 0.0f;
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        if (!(st.wave[i][0] > 0.0f)) continue;
        const float k0 = 6.2831853f / st.wave[i][0];
        const float k  = oceanWaveNumber(k0, depthM);
        const float amp = oceanAmp(st, i, fetchM) * green * scale * fade * oceanShortWaveFade(k, quadM);
        if (amp <= 1e-4f) continue;
        const glm::vec3 D = oceanRefract(glm::normalize(t1 * st.wave[i][2] + t2 * st.wave[i][3]),
                                         upSlope, up, k0, k);
        const float ph = oceanTrainPhase(st, i, k0, k, D, wp, t1, t2, t, bedM);
        if (i == 0) Dl = D;
        const float dd = glm::dot(D, Dl);
        gradDl += amp * k * std::cos(ph) * dd;
        hess   += D * (amp * k * k * std::sin(ph) * dd);
        sumAK  += amp * k;
    }
    if (!(sumAK > 0.0f) || glm::dot(Dl, Dl) < 0.5f) return g;
    // Suave (C1) y entera ya a `OCEAN_CURL_GATE_FR`: con `G = fr` a secas la cara delantera rara vez
    // pasaba de ~0,3 y el adelanto se quedaba en un tercio — la rompiente dejo de plegarse
    // (`ocean_break_fold`). La rampa suave es para que la divergencia (`gradG`) no tenga saltos.
    const float fr = -gradDl / sumAK;
    const float x  = glm::clamp(fr / OCEAN_CURL_GATE_FR, 0.0f, 1.0f);
    g.G = x * x * (3.0f - 2.0f * x);
    if (x > 0.0f && x < 1.0f) g.gradG = hess * (6.0f * x * (1.0f - x) / (OCEAN_CURL_GATE_FR * sumAK));
    return g;
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
/// ⚠️ Gemelos de `HARUKA_BORE_*`. La espuma de ROMPIENTE va con la FASE de la ola compuesta: blanca en
/// la cresta que revienta (`e = η/Σamp` sobre `OCEAN_BORE_LO..HI`) y, entre una y otra, el resto de
/// espuma gastada (`OCEAN_SPENT_FOAM`). Ver `oceanFoam`.
inline constexpr float OCEAN_BORE_LO    = -0.20f;
inline constexpr float OCEAN_BORE_HI    =  0.60f;
inline constexpr float OCEAN_SPENT_FOAM =  0.25f;

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
                           float fetchM = WATER_FETCH_UNLIMITED,
                             float quadM = OCEAN_MIN_QUAD_M) {
    const float green = oceanGreenGain(depthM) * oceanFetchFactor(st, fetchM);
    const float scale = oceanBreakScale(depthM, st, fetchM, quadM);
    float best = 0.0f, bestAmp = 0.0f;
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        if (!(st.wave[i][0] > 0.0f)) continue;
        const float k0 = 6.2831853f / st.wave[i][0];
        const float k  = oceanWaveNumber(k0, depthM);
        const float amp = oceanAmp(st, i, fetchM) * green * scale * oceanShortWaveFade(k, quadM);
        if (amp > bestAmp) { bestAmp = amp; best = oceanStokesSkew(k, amp, depthM); }
    }
    return best;
}

inline float oceanSteepScale(float depthM, const OceanState& st,
                             float fetchM = WATER_FETCH_UNLIMITED,
                             float quadM = OCEAN_MIN_QUAD_M) {
    const float green = oceanGreenGain(depthM) * oceanFetchFactor(st, fetchM);
    const float brk   = oceanBreakScale(depthM, st, fetchM, quadM);
    float sum = 0.0f;
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        const float k0 = 6.2831853f / st.wave[i][0];
        const float k  = oceanWaveNumber(k0, depthM);
        const float amp = oceanAmp(st, i, fetchM) * green * brk * oceanShortWaveFade(k, quadM);
        sum += k * oceanHorizAmp(k, amp, depthM);
    }
    // ⚠️ EL TECHO SUBE DONDE LA OLA ROMPE, y ese es el punto entero. Con el techo fijo en 0,75 la
    // cresta no puede plegarse NUNCA, que es lo que se midio. Donde `breakiness` dice que el limite
    // de rompiente esta mordiendo —o sea donde una ola de verdad revienta— se le deja pasar de 1.
    // Fuera de esa franja el techo sigue siendo el de antes, asi que el mar abierto y el agua
    // interior quieta no cambian ni un bit.
    const float ceiling = OCEAN_MAX_STEEP
                        + (1.0f + OCEAN_CURL_MAX - OCEAN_MAX_STEEP) * oceanCurlGain(depthM, st, fetchM, quadM)
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
                        const OceanState& st, float fetchM = WATER_FETCH_UNLIMITED,
                        float bedM = OCEAN_NO_BED) {
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
    // ⚠️ LA AMPLITUD ES LA DEL MAR ENTERO (`sqrt(Σa²)`), NO LA DEL TREN 0. La fase sigue al tren largo
    // —es el que marca el ritmo de la trepada—, pero con viento flojo ese tren casi no tiene energia
    // y la orilla se quedaba quieta aunque el agua de al lado subiera 70 cm (`coast_surf_moves`).
    float sum2 = 0.0f;
    // En agua cerrada, la del mar de viento con su fetch: sin esto la orilla de un lago respiraria
    // con la mar de fondo del oceano.
    for (int i = 0; i < OCEAN_WAVES; ++i) { const float a = oceanAmp(st, i, fetchM); sum2 += a * a; }
    const float aBreak = oceanShoalAmp(std::max(depthRest, 1.0f),
                                       std::sqrt(sum2) * oceanFetchFactor(st, fetchM));
    const float reach  = std::max(aBreak * 6.0f, 2.0f);
    const float wgt    = 1.0f - glm::smoothstep(0.0f, reach, std::max(depthRest, 0.0f));
    // Cuarto de ciclo de retraso respecto a la cresta: el agua sube DESPUÉS de que rompa.
    const float ph = oceanTrainPhase(st, 0, kDeep0, k0, D0, wp, t1, t2, t, bedM) - 1.5707963f;
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
                                   const glm::vec3& upSlope = glm::vec3(0.0f),
                                   float quadM = OCEAN_MIN_QUAD_M,
                                   float bedM = OCEAN_NO_BED) {
    const glm::vec3 t1 = glm::normalize(std::abs(up.y) < 0.99f ? glm::cross(up, glm::vec3(0, 1, 0))
                                                               : glm::cross(up, glm::vec3(1, 0, 0)));
    const glm::vec3 t2 = glm::cross(up, t1);
    const float green      = oceanGreenGain(depthM) * oceanFetchFactor(st, fetchM);
    const float scale      = oceanBreakScale(depthM, st, fetchM, quadM);
    const float steepScale = oceanSteepScale(depthM, st, fetchM, quadM);
    const float curl       = oceanCurlGain(depthM, st, fetchM, quadM);
    const float breaker    = oceanBreakerGain(depthM, st, fetchM, quadM);
    const float lipGain    = oceanLipGain(depthM, st, fetchM, quadM);
    const float gate       = (curl > 0.0f) ? oceanCurlGate(wp, up, t1, t2, t, depthM, fade, st, fetchM,
                                                           upSlope, quadM, green, scale, bedM).G : 0.0f;
    OceanLipAcc lip;
    glm::vec3 disp(0.0f);
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        if (!(st.wave[i][0] > 0.0f)) continue;
        const float k0 = 6.2831853f / st.wave[i][0];
        const float k  = oceanWaveNumber(k0, depthM);
        const float amp = oceanAmp(st, i, fetchM) * green * scale * fade * oceanShortWaveFade(k, quadM);
        if (amp <= 1e-4f) continue;
        const glm::vec3 D = oceanRefract(glm::normalize(t1 * st.wave[i][2] + t2 * st.wave[i][3]),
                                         upSlope, up, k0, k);
        const float w  = std::sqrt(OCEAN_G * k0);
        const float ph = oceanTrainPhase(st, i, k0, k, D, wp, t1, t2, t, bedM);
        const float c = std::cos(ph), s = std::sin(ph);
        // ⚠️ EL REFUERZO DE ROMPIENTE VA SOLO AL TREN 0, Y ESE ES EL PUNTO. Repartido entre los ocho
        // comprime en ocho direcciones y el determinante nunca llega a 0; concentrado en el tren que
        // rompe —refractado, o sea ya apuntando a la orilla— toda la compresión cae en un eje, que es
        // como se organiza un surf real. Ver `oceanBreakerGain`.
        // ⚠️ EL ADELANTO VA EN LA MITAD DELANTERA, `cos φ < 0` (17-10): la ola viaja hacia +D y la
        // cresta esta en `sin φ = 1`. Con `max(0, cos φ)` empinaba la cara de ATRAS (medido en el
        // corte de la rompiente; `coast_lip_front_steep`).
        const float hBase = oceanHorizAmp(k, amp, depthM) * steepScale
                          * ((i == 0) ? (1.0f + breaker) : 1.0f);
        const float curlI = oceanCurlCap(curl, hBase, amp);             // ver `oceanCurlCap`
        const float horiz = hBase * (1.0f + curlI * gate * std::max(0.0f, -c));   // ver `oceanCurlGate`
        // El 2º armónico va en la VERTICAL: cresta picuda, seno plano. POR TREN, del escarpado y el
        // fondo de ESE tren (`oceanStokesSkew`), no de una constante común a los ocho.
        const float skew = oceanStokesSkew(k, amp, depthM);
        disp += D * (horiz * c) + up * (amp * (s - skew * std::cos(2.0f * ph)));
        lip.add(i, D, amp, k, c, s);
    }
    // EL LABIO, sobre la ola compuesta (ver `oceanLipGain`).
    if (lipGain > 0.0f) disp += lip.disp(up, lipGain);
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
                           float* outSlope = nullptr, float* outSigma = nullptr,
                           float quadM = OCEAN_MIN_QUAD_M, float* outCrest = nullptr,
                           float bedM = OCEAN_NO_BED) {
    // MISMO marco tangente que `oceanWaveHeight`, `oceanWaveVelocity` y el shader.
    const glm::vec3 t1 = glm::normalize(std::abs(up.y) < 0.99f ? glm::cross(up, glm::vec3(0, 1, 0))
                                                               : glm::cross(up, glm::vec3(1, 0, 0)));
    const glm::vec3 t2 = glm::cross(up, t1);
    const float green      = oceanGreenGain(depthM) * oceanFetchFactor(st, fetchM);
    const float scale      = oceanBreakScale(depthM, st, fetchM, quadM);
    const float steepScale = oceanSteepScale(depthM, st, fetchM, quadM);
    const float curl       = oceanCurlGain(depthM, st, fetchM, quadM);
    const float breaker    = oceanBreakerGain(depthM, st, fetchM, quadM);
    const float lipGain    = oceanLipGain(depthM, st, fetchM, quadM);
    const OceanCurlGate gate = (curl > 0.0f) ? oceanCurlGate(wp, up, t1, t2, t, depthM, fade, st, fetchM,
                                                             upSlope, quadM, green, scale, bedM)
                                             : OceanCurlGate{};
    glm::vec3 curlVec(0.0f);   // Σ D·(H·adelanto·max(0,−cos φ)·cos φ): lo que multiplica a ∇G en la divergencia
    OceanLipAcc lip;
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
        const float amp = oceanAmp(st, i, fetchM) * green * scale * fade * oceanShortWaveFade(k, quadM);
        if (amp <= 1e-4f) continue;
        const glm::vec3 D = oceanRefract(glm::normalize(t1 * st.wave[i][2] + t2 * st.wave[i][3]),
                                         upSlope, up, k0, k);
        const float w  = std::sqrt(OCEAN_G * k0);       // la frecuencia NO cambia con el fondo
        const float ph = oceanTrainPhase(st, i, k0, k, D, wp, t1, t2, t, bedM);
        const float c = std::cos(ph), s = std::sin(ph);
        // ⚠️ AQUI HABIA UN ERROR DE DERIVADA, Y ERA EL QUE MANTENIA APAGADA LA ESPUMA DE PLEGADO.
        // El desplazamiento a lo largo de `D` es `H·cos φ·(1 + g·max(0,−cos φ))`, o sea
        // `H·(cos φ − g·cos²φ)` en la mitad delantera (`cos φ < 0`: ver la nota de `oceanDisplacement`).
        // Su derivada respecto a la fase es `−H·sin φ·(1 − 2g·cos φ)` — con un DOS, porque el termino
        // del adelanto es cuadratico en `cos φ`. El motor acumulaba `(1 + g·cos φ)`: se dejaba la mitad del
        // efecto del adelanto justo en la magnitud que decide si la ola rompe.
        //
        // Medido con la derivada NUMERICA de `oceanDisplacement` como oraculo: el peor
        // `|analitico − traza|` era **0,3071**. No es un ajuste fino: es que la formula no era la
        // derivada de lo que el motor dibuja.
        const float hBase = oceanHorizAmp(k, amp, depthM) * steepScale
                          * ((i == 0) ? (1.0f + breaker) : 1.0f);   // ver `oceanBreakerGain`
        const float curlI  = oceanCurlCap(curl, hBase, amp);            // ver `oceanCurlCap`
        const float dHoriz = hBase * (1.0f + 2.0f * curlI * gate.G * std::max(0.0f, -c));
        jac -= dHoriz * k * s;
        curlVec += D * (hBase * curlI * std::max(0.0f, -c) * c);
        lip.add(i, D, amp, k, c, s);
        const float ka = amp * k;
        grad += glm::vec2(glm::dot(D, t1), glm::dot(D, t2)) * (ka * c);
        sumSq += ka * ka;                      // σ de la pendiente: escala de la distribucion
    }
    // La puerta del adelanto cambia de un punto a otro: su gradiente también entra en la divergencia.
    jac += glm::dot(curlVec, gate.gradG);
    // El labio adelanta la cresta a su cara: en la cara delantera `∂(L·u)/∂Dl < 0` y la lamina pliega.
    if (lipGain > 0.0f) jac += lip.jacTerm(lipGain);
    if (outSlope) *outSlope = glm::length(grad);
    if (outSigma) *outSigma = std::sqrt(sumSq * 0.5f);
    // Elevacion de la ola COMPUESTA en unidades de su amplitud (−1 seno · +1 cresta): la fase que
    // sigue la espuma de rompiente.
    if (outCrest) *outCrest = (lip.sumA > 0.0f) ? lip.eta / lip.sumA : 0.0f;
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
                       const glm::vec3& upSlope = glm::vec3(0.0f),
                                   float quadM = OCEAN_MIN_QUAD_M,
                                   float bedM = OCEAN_NO_BED) {
    float slope = 0.0f, sigma = 0.0f, crest = 0.0f;
    const float jac = oceanJacobian(wp, up, t, depthM, fade, st, fetchM, upSlope, &slope, &sigma, quadM, &crest, bedM);
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
    // ⚠️ Y VA CON LA FASE DE LA OLA (17-10). El indice solo dice CUANTO rompe este sitio, no CUANDO:
    // usado tal cual, todo lo mas somero que ~2 m salia blanco opaco y QUIETO, con el borde siguiendo
    // los triangulos del agua (captura en 31,09 · 126,07: la orilla entera una mancha blanca, sin una
    // sola ola). El agua blanca de verdad viaja con la cresta que revienta y deja detras espuma
    // gastada, asi que se modula con la elevacion de la ola compuesta: 1 en la cresta, el resto
    // (`OCEAN_SPENT_FOAM`) en el seno. `ocean_foam` mide que la orilla siga mas blanca que el mar
    // abierto y que ahora haya bandas que se mueven.
    const float bore = glm::smoothstep(OCEAN_BORE_LO, OCEAN_BORE_HI, crest);
    const float shoreFoam = glm::smoothstep(0.78f, 1.60f, oceanBreakIndex(depthM, st, fetchM, quadM))
                          * (OCEAN_SPENT_FOAM + (1.0f - OCEAN_SPENT_FOAM) * bore);
    // ⚠️ LA ESCALA VA AQUI TAMBIEN, y faltaba: `harukaGerstner` multiplica su espuma por
    // `harukaFoamScale()` (el `misc.w` del UBO, que sale de este mismo `oceanFoamScale`) y este
    // gemelo no lo hacia. Con la escala por defecto (1,0) daba igual, pero `HARUKA_OCEAN_FOAM=0.5`
    // dejaba la espuma que se DIBUJA a la mitad de la que se CONSULTA — dos respuestas para la misma
    // pregunta, que es justo lo que el careo GPU<->CPU existe para impedir.
    return glm::clamp(std::max(capFoam, shoreFoam) * fade * oceanFoamScale(), 0.0f, 1.0f);
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
                             const glm::vec3& upSlope = glm::vec3(0.0f),
                                   float quadM = OCEAN_MIN_QUAD_M,
                                   float bedM = OCEAN_NO_BED) {
    const glm::vec3 t1 = glm::normalize(std::abs(up.y) < 0.99f ? glm::cross(up, glm::vec3(0, 1, 0))
                                                               : glm::cross(up, glm::vec3(1, 0, 0)));
    const glm::vec3 t2 = glm::cross(up, t1);
    // Bajío y tope de rompiente: el bajío es por tren (cada longitud de onda crece a su ritmo) pero
    // el tope es COMÚN a los cuatro, porque el índice de rompiente habla de la ola entera.
    // El FETCH entra multiplicando el bajío: es cuánta ola puede haber aquí antes de que el fondo la
    // amplifique. Sin él, un lago hereda el swell del océano — ver `oceanFetchFactor`.
    const float green = oceanGreenGain(depthM) * oceanFetchFactor(st, fetchM);
    const float scale = oceanBreakScale(depthM, st, fetchM, quadM);
    // La CAIDA del labio (`oceanLipGain`) tambien: es vertical, y lo que se nada es lo que se ve.
    const float lipGain = oceanLipGain(depthM, st, fetchM, quadM);
    OceanLipAcc lip;
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
        const float amp = oceanAmp(st, i, fetchM) * green * scale * fade * oceanShortWaveFade(k, quadM);
        if (amp <= 1e-4f) continue;
        // ⚠️ REFRACTADA. La direccion de la tabla es la de AGUAS PROFUNDAS; al perder fondo la ola
        // gira hacia la orilla (`oceanRefract`). Sin pendiente el vector sale identico.
        const glm::vec3 D = oceanRefract(glm::normalize(t1 * st.wave[i][2] + t2 * st.wave[i][3]),
                                         upSlope, up, k0, k);
        const float w  = std::sqrt(OCEAN_G * k0);         // la frecuencia NO cambia con el fondo
        const float ph = oceanTrainPhase(st, i, k0, k, D, wp, t1, t2, t, bedM);
        // Componente a lo largo de `up`, con el 2º armonico POR TREN. No cambia la altura pico-valle,
        // asi que el limite de rompiente y la flotabilidad siguen significando lo mismo.
        h += amp * (std::sin(ph) - oceanStokesSkew(k, amp, depthM) * std::cos(2.0f * ph));
        lip.add(i, D, amp, k, std::cos(ph), std::sin(ph));
    }
    if (lipGain > 0.0f) h += glm::dot(lip.disp(up, lipGain), up);
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
                                   const glm::vec3& upSlope = glm::vec3(0.0f),
                                   float quadM = OCEAN_MIN_QUAD_M,
                                   float bedM = OCEAN_NO_BED) {
    // MISMO marco que `oceanWaveHeight` y que el shader. Si estas dos líneas divergen, la velocidad
    // apunta a otro sitio que la altura y el agua empuja en diagonal respecto a sus propias crestas.
    const glm::vec3 t1 = glm::normalize(std::abs(up.y) < 0.99f ? glm::cross(up, glm::vec3(0, 1, 0))
                                                               : glm::cross(up, glm::vec3(1, 0, 0)));
    const glm::vec3 t2 = glm::cross(up, t1);
    // MISMO bajío, MISMO tope común y MISMO escarpado que `oceanWaveHeight`. Si estas tres líneas se
    // separaran de las suyas, la velocidad dejaría de ser la derivada de la altura y el test de
    // paridad lo cazaría — que es justo para lo que está.
    const float green      = oceanGreenGain(depthM) * oceanFetchFactor(st, fetchM);
    const float scale      = oceanBreakScale(depthM, st, fetchM, quadM);
    const float breakiness = glm::clamp(1.0f - scale, 0.0f, 1.0f);
    const float steepScale = oceanSteepScale(depthM, st, fetchM, quadM);
    const float curl       = oceanCurlGain(depthM, st, fetchM, quadM);
    const float breaker    = oceanBreakerGain(depthM, st, fetchM, quadM);
    const float gate       = (curl > 0.0f) ? oceanCurlGate(wp, up, t1, t2, t, depthM, fade, st, fetchM,
                                                           upSlope, quadM, green, scale, bedM).G : 0.0f;
    (void)breakiness;
    glm::vec3 v(0.0f);
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        // Mismas tres líneas que `oceanWaveHeight`, y por el mismo motivo: si se separaran, la
        // velocidad dejaría de ser la derivada de la altura y `ocean_wave_velocity` lo cazaría.
        const float k0 = 6.2831853f / st.wave[i][0];
        const float k  = oceanWaveNumber(k0, depthM);
        const float amp = oceanAmp(st, i, fetchM) * green * scale * fade * oceanShortWaveFade(k, quadM);
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
        const float ph = oceanTrainPhase(st, i, k0, k, D, wp, t1, t2, t, bedM);
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
        const float hBase = oceanHorizAmp(k, amp, depthM) * steepScale
                          * ((i == 0) ? (1.0f + breaker) : 1.0f);   // ver `oceanBreakerGain`
        // ⚠️ Era `max(0, cos φ)`: el signo VIEJO del adelanto (la mitad de atrás), que
        // `oceanDisplacement` ya había corregido. `∂/∂t` de `H·(cos φ − g·cos²φ)` lleva el DOS, como
        // el jacobiano. Y con la puerta de la ola compuesta y el tope por tren (`oceanCurlCap`).
        const float horiz = hBase * (1.0f + 2.0f * oceanCurlCap(curl, hBase, amp) * gate * std::max(0.0f, -cph));
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

} // namespace Haruka::World::Water
