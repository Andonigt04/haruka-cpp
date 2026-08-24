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

/// Gemelo de `harukaShoalAmp`: ley de Green + límite de rompiente (la ola no crece, revienta).
inline float oceanShoalAmp(float depthM, float ampDeep) {
    const float d     = std::max(depthM, 0.05f);
    const float green = std::pow(std::clamp(50.0f / d, 1.0f, 40.0f), 0.25f);
    return std::min(ampDeep * green, 0.55f * depthM);
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
    const float k0 = 6.2831853f / lambda0;
    const float w0 = std::sqrt(OCEAN_G * k0);
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
                             float depthM, float fade, const OceanState& st) {
    const glm::vec3 t1 = glm::normalize(std::abs(up.y) < 0.99f ? glm::cross(up, glm::vec3(0, 1, 0))
                                                               : glm::cross(up, glm::vec3(1, 0, 0)));
    const glm::vec3 t2 = glm::cross(up, t1);
    float h = 0.0f;
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        const float lambda = st.wave[i][0];
        const float k      = 6.2831853f / lambda;
        const float amp    = oceanShoalAmp(depthM, st.wave[i][1]) * fade;
        if (amp <= 1e-4f) continue;
        const glm::vec3 D = glm::normalize(t1 * st.wave[i][2] + t2 * st.wave[i][3]);
        const float w  = std::sqrt(OCEAN_G * k);          // dispersión de aguas profundas
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
                                   float depthM, float fade, const OceanState& st) {
    // MISMO marco que `oceanWaveHeight` y que el shader. Si estas dos líneas divergen, la velocidad
    // apunta a otro sitio que la altura y el agua empuja en diagonal respecto a sus propias crestas.
    const glm::vec3 t1 = glm::normalize(std::abs(up.y) < 0.99f ? glm::cross(up, glm::vec3(0, 1, 0))
                                                               : glm::cross(up, glm::vec3(1, 0, 0)));
    const glm::vec3 t2 = glm::cross(up, t1);
    glm::vec3 v(0.0f);
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        const float lambda = st.wave[i][0];
        const float k      = 6.2831853f / lambda;
        const float amp    = oceanShoalAmp(depthM, st.wave[i][1]) * fade;
        if (amp <= 1e-4f) continue;
        const glm::vec3 D = glm::normalize(t1 * st.wave[i][2] + t2 * st.wave[i][3]);
        const float w  = std::sqrt(OCEAN_G * k);          // dispersión de aguas profundas
        const float ph = k * glm::dot(D, wp) - w * t;
        // Q = escarpado, GEMELO EXACTO de `ocean_wave.glsl` (misma fórmula, mismo tope). Entra en la
        // velocidad horizontal porque es quien reparte cuánto del círculo es avance y cuánto subida.
        float Q = 0.75f / (k * amp * float(OCEAN_WAVES) + 1e-4f);
        Q = std::min(Q, 1.0f);
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
