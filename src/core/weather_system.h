/**
 * @file weather_system.h
 * @brief EL CLIMA COMO ESTADO DEL MUNDO — frentes con posición y deriva sobre el planeta.
 *
 * ── EL PROBLEMA QUE RESUELVE ────────────────────────────────────────────────────────────────────
 * El clima vivía DENTRO del shader del cielo, y la lluvia la decidía la CPU por su cuenta. Eran dos
 * relojes distintos evaluando dos fórmulas distintas:
 *
 *   CPU (application_render.cpp) : cloudy = 0.15 + 0.45·(0.5+0.5·sin(t·0.05)) + 0.55·humedad
 *   GPU (sky.frag)               : cloudiness = 0.16 + 0.38·fbm(t·0.008) + 0.5·humedad + 0.7·lluvia
 *
 * Un seno contra un fBm. No hay ningún motivo por el que deban coincidir, y no coincidían: llovía
 * con el cielo despejado y salían nubarrones sin una gota. El síntoma que reportó el autor —"la
 * lluvia sale donde sea, no es lluvia que cae de las nubes"— es LITERALMENTE eso: la lluvia nunca
 * supo dónde estaban las nubes, porque nadie era dueño de esa información.
 *
 * ── LA DEFINICIÓN ───────────────────────────────────────────────────────────────────────────────
 * El clima es una FUNCIÓN PURA de (seed, tiempo, dirección) — no hay estado acumulado, no hay
 * integración. `kFronts` frentes recorren el planeta girando cada uno alrededor de su propio eje;
 * la cobertura en un punto es la suma de los frentes que lo tapan, modulada por la humedad del campo
 * (un frente que cruza la selva descarga; sobre el desierto pasa de largo casi seco).
 *
 *   · Determinista: dos clientes con la misma seed y el mismo reloj ven LA MISMA tormenta, y el DGS
 *     puede validarla sin replicar nada. Es el requisito de multijugador, y sale gratis por ser pura.
 *   · GL-free y en `haruka_simbase` (como `reference_surface`) justo por eso.
 *   · La PRECIPITACIÓN SALE DE LA COBERTURA, no de un reloj aparte: `precip > 0 ⇒ cloudCover` alto,
 *     por construcción. Esa implicación es el invariante del sistema y tiene test
 *     (`test_weather_fronts`): sin nube encima no puede llover. Nunca más.
 *
 * ── QUIÉN LO LEE ────────────────────────────────────────────────────────────────────────────────
 * El cielo (cobertura y color de nube), la precipitación (cuánta y de qué tipo), el suelo (mojado,
 * nieve acumulada) y el viento (nubes, follaje, lluvia inclinada — hoy cada uno usa su reloj). Todos
 * la MISMA fuente: si el cielo dice cubierto, llueve; si dice despejado, no.
 *
 * ── LO QUE NO ES ────────────────────────────────────────────────────────────────────────────────
 * No es una simulación atmosférica. No hay advección ni conservación de masa: es un campo de frentes
 * que se mueven, con la forma fina de la nube resuelta en el shader (que sí puede permitirse ruido
 * por píxel). La CPU dice CUÁNTA nube y DÓNDE; el shader dice QUÉ FORMA tiene.
 */
#pragma once

#include <glm/glm.hpp>
#include <cstdint>

namespace Haruka {

/** @brief Qué cae del cielo. La temperatura del punto decide entre lluvia y nieve. */
enum class Precip : uint8_t {
    None = 0,
    Rain = 1,
    Snow = 2,
};

/** @brief El clima en un punto del planeta, en un instante. Todo derivado, nada guardado. */
struct WeatherSample {
    float  cloudCover = 0.0f;    ///< [0,1] cuánta nube hay ENCIMA de este punto.
    float  precip     = 0.0f;    ///< [0,1] intensidad de precipitación. >0 exige cloudCover alto.
    Precip type       = Precip::None;
    float  cloudBaseM = 1200.0f; ///< altura de la base de la nube (m sobre el suelo). Techo de la lluvia.
    /// TECHO de la nube (m sobre el suelo). Sin esto la nube es una SUPERFICIE, no un cuerpo: el
    /// clima se lee bien desde el suelo y deja de existir en cuanto despegas. Con base y techo la
    /// nube es una losa que se puede atravesar, ver de canto desde un avión y dejar por debajo.
    /// El grosor no es constante: una capa de buen tiempo tiene cientos de metros y una tormenta
    /// crece kilómetros (el cumulonimbo se desarrolla en vertical, y por eso descarga).
    float  cloudTopM  = 1800.0f;
    float  tempC      = 15.0f;   ///< temperatura del campo en el punto (la que decide lluvia/nieve).
    float  humidity   = 0.5f;    ///< humedad del campo en el punto.
    glm::vec3 wind{0.0f};        ///< viento TANGENTE (m/s) en el marco local del planeta.

    /// Grosor de la nube (m). >0 siempre que haya algo de cobertura.
    float cloudThicknessM() const { return cloudTopM - cloudBaseM; }

    bool isSnow() const { return type == Precip::Snow; }
    /** @brief Intensidad de LLUVIA (0 si lo que cae es nieve). Para el pase de lluvia y el mojado. */
    float rainAmount() const { return type == Precip::Rain ? precip : 0.0f; }
    /** @brief Intensidad de NIEVE (0 si lo que cae es lluvia). Para la acumulación en el suelo. */
    float snowAmount() const { return type == Precip::Snow ? precip : 0.0f; }
};

class WeatherSystem {
public:
    /** @brief Nº de frentes que recorren el planeta. Contado, no "muchos": cada uno debe LEERSE como
     *  un sistema con su borde que llega y pasa, no como ruido. Con 14 (y radios de 0.25–0.72 rad =
     *  1600–4600 km sobre la Tierra) sale un planeta parecido al real —mayoría de cielo con algo de
     *  nube, y a la vez SIEMPRE zonas despejadas, que el test asevera. */
    static constexpr int kFronts = 14;

    /** @brief Segundos de simulación que tarda un frente en dar la vuelta al planeta (por defecto).
     *  Es EL knob del ritmo: subirlo = clima más lento y de mayor escala. 1800 s ≈ media hora de
     *  juego para cruzar el mundo → una tormenta tarda unos minutos en llegar y en pasar. */
    static constexpr double kFrontRevolutionS = 1800.0;

    /** @brief Por debajo de esta temperatura lo que cae es NIEVE. */
    static constexpr float kSnowTempC = 1.0f;

    /** @brief Aportación MÁXIMA de un frente al viento, en m/s.
     *
     *  ⚠️ AQUÍ ESTABA EL BUG DE LOS 969 m/s. El término de arrastre era `ω·R·0.35`, o sea la
     *  velocidad de AVANCE del frente sobre el suelo — y esa velocidad no es física, es de juego: un
     *  frente da la vuelta al planeta en `kFrontRevolutionS` = 1800 s, que sobre 6371 km son
     *  **22 km/s** de avance. Con el factor 0.35 quedaban 7,8 km/s por frente y el bucle SUMA los
     *  14 sin techo alguno. Medido en el juego: `[Mar] viento 968.9 m/s`.
     *
     *  El mar no lo notaba porque `oceanStateFromWind` topa U en [4, 22] — el clamp tapaba el bug en
     *  el único consumidor que lo habría delatado. Cualquier otro (follaje, lluvia inclinada, y desde
     *  hoy los vórtices, que se disparan con el viento) lo veía crudo.
     *
     *  El arreglo NO es escalar el factor: la velocidad del frente no debe decidir la del viento en
     *  absoluto, porque el ritmo del clima es un mando de diseño (`kFrontRevolutionS`) y tocarlo no
     *  puede convertir una brisa en un huracán. Lo que sí se conserva es el SIGNO (los frentes que
     *  van al revés soplan al revés) y el PESO (el viento arrecia hacia el núcleo), que es lo que se
     *  nota al jugar: primero cambia el viento, luego llega la nube. */
    static constexpr float kFrontWindMS = 22.0f;

    /** @brief Techo del viento SOSTENIDO (m/s). 45 m/s ≈ 162 km/h, un huracán de categoría 2: por
     *  encima de eso el aire libre no sopla en la Tierra, y lo que sí sopla más (un tornado) no es
     *  viento de fondo sino un VÓRTICE — vive en `Vortex` y se suma aparte, justo para que el campo
     *  de fondo no tenga que estirarse hasta los 100 m/s y arrastre con él a todo el planeta. */
    static constexpr float kMaxWindMS = 45.0f;

    /** @brief Cobertura a partir de la cual el frente descarga. Debajo hay nube pero no llueve —
     *  que es justo la mitad de "la lluvia cae de las nubes": nube sin lluvia SÍ existe.
     *
     *  ⚠️ SUBIÓ de 0.62 a 0.78 POR ARRASTRE, no por decisión de balance. La lluvia sale de la
     *  cobertura, así que al corregir el planeta despejado (mediana 0,111 → 0,30) TODO el mundo
     *  cruzaba este umbral y la frecuencia de lluvia se disparaba sola. Medido a humedad 0,65:
     *  llovía el 11,0 % del tiempo, con la cobertura nueva y este umbral sin tocar habría sido el
     *  27,9 %, y con 0.78 queda en el 21,3 %.
     *
     *  ⚠️ **No se puede volver al 11 % sin perder las nubes**: subir más este umbral lo mete en la
     *  cola de `smoothstep(kPrecipCover, 0.94, cover)`, o sea que la lluvia pasaría de nada a todo
     *  en una franja estrechísima. Que un cielo más nublado llueva más es la física del sistema
     *  —`precip` no tiene otra fuente—, así que si el 21 % molesta, el mando NO es este umbral sino
     *  el término `0.30 + 0.70·H` de `sampleAt`, que gradúa la INTENSIDAD. */
    static constexpr float kPrecipCover = 0.78f;

    /** @brief Bordes de la losa de nube, en fracción del grosor: dónde acaba de entrar y dónde
     *  empieza a deshilacharse.
     *
     *  ⚠️ ESTÁN DUPLICADOS EN `assets/shaders/lib/cloud_volume.glsl` (`HARUKA_CLOUD_RISE`/`_FALL`),
     *  porque el mismo volumen se evalúa en dos sitios: aquí para saber si el ojo está dentro, y en
     *  el shader para pintarlo. No hay forma de compartir un `constexpr` con GLSL, así que los ata
     *  un TEST que lee el fichero de shader (`weather_3d`). Sin esa atadura, divergir es cuestión de
     *  tiempo — y el síntoma sería entrar en la niebla a una altura distinta de la que se ve la
     *  nube, que es el mismo fallo que tenía el clima cuando CPU y shader calculaban la nubosidad
     *  cada uno por su cuenta. */
    static constexpr float kProfileRise = 0.28f;
    static constexpr float kProfileFall = 0.62f;

    /** @brief Escala del campo horizontal de nube, en 1/metros. Su inversa es el ANCHO típico de un
     *  cúmulo (0.0007 ⇒ ~1430 m).
     *
     *  ⚠️ Vive aquí, y no como literal en el pase de render, porque forma PAREJA con el grosor que
     *  calcula `sampleAt`: juntos deciden si una nube se lee como cuerpo o como lámina. Con la
     *  escala vieja (0.00035 ⇒ 2857 m) y el cuerpo viejo (260 + 900·cover ⇒ 440-980 m) el cúmulo de
     *  buen tiempo era 3-6 veces más ancho que alto, y eso se ve exactamente como lo que se
     *  reportó: una sábana. Un test (`cloud_shape`) fija la relación para que un retoque futuro de
     *  cualquiera de los dos no la rompa en silencio. */
    static constexpr float kFieldScale = 0.0007f;

    /** @brief Ancho típico de un cúmulo en metros: la inversa de la escala del campo. */
    static constexpr float cloudWidthM() { return 1.0f / kFieldScale; }

    /** @brief Fija la seed del planeta y siembra los frentes. Idempotente para la misma seed. */
    void configure(uint32_t seed);

    /** @brief Reloj del mundo (segundos de simulación; `PlanetarySystem::simulationTime()`).
     *  NO es un `steady_clock`: tiene que ser el mismo número en el cliente y en el servidor. */
    void setTime(double seconds) { m_time = seconds; }
    double time() const { return m_time; }
    uint32_t seed() const { return m_seed; }

    /** @brief Escala del ritmo del clima (1 = `kFrontRevolutionS`). >1 acelera (para depurar). */
    void setTimeScale(double s) { m_timeScale = (s > 0.0) ? s : 1.0; }
    double timeScale() const { return m_timeScale; }

    /**
     * @brief El clima en una dirección del planeta.
     * @param dir      dirección UNITARIA desde el centro del planeta (marco local del planeta).
     * @param tempC    temperatura del CAMPO en ese punto (`TerrainSample::tempC`).
     * @param humidity humedad del CAMPO en ese punto (`TerrainSample::humidity`).
     *
     * El clima no se inventa la temperatura ni la humedad: las toma del campo del planeta, que ya
     * las deriva de latitud/altitud y de la advección desde el mar con sombra orográfica. El sistema
     * solo aporta el EVENTO (el frente que pasa) sobre ese fondo.
     */
    WeatherSample sampleAt(const glm::dvec3& dir, float tempC, float humidity) const;

    /** @brief Solo la cobertura de nube (más barato: sin viento ni base de nube). */
    float cloudCoverAt(const glm::dvec3& dir, float humidity) const;

    /**
     * @brief EL CIELO ENTERO, HORNEADO: por dirección, (cobertura, base m, techo m, precipitación).
     *
     * ⚠️ ANTES SE HORNEABA SÓLO LA COBERTURA, y la base, el techo y la lluvia salían de UN punto: el
     * que la cámara tiene debajo. Consecuencia: todo el planeta tenía la misma losa de nube —el mismo
     * grosor sobre la tormenta de 300 km que sobre el claro de al lado— y el pase compensaba con
     * CAPAS FIJAS (altocúmulo a 4 km, cirro a 8 km, siempre, a una fracción de la cobertura). Eso
     * es exactamente "nubes por capas" en vez de "nubes por lo que decida el clima". Con los cuatro
     * canales por dirección, el shader lee en cada punto la nube que ESE punto tiene: fina y baja
     * en buen tiempo, torre de 6 km donde descarga, y las capas altas salen de condiciones (cirro
     * donde hay cobertura sin lluvia, yunque donde hay torre), no de una altura fija.
     *
     * Equirect W×H con la convención del motor (la misma que lee `cloud_vol.frag`): fila 0 = polo
     * norte, columna 0 = longitud −180°. `tempC` y `humidity` son campos estáticos del terreno del
     * mismo tamaño (salen del bioma; se hornean una vez fuera y se reutilizan).
     *
     * @param outRGBA   W·H·4 floats: r = cobertura · g = base (m) · b = techo (m) · a = precipitación
     * @return          la cobertura MÁXIMA del planeta (para la salida rápida del pase)
     */
    float bakeSky(const float* tempC, const float* humidity, int W, int H,
                  float* outRGBA, float* outBaseMin = nullptr, float* outTopMax = nullptr,
                  const float* water = nullptr, float* outHiRGBA = nullptr) const;

    /** @brief Humedad EFECTIVA para el cielo sobre el agua.
     *
     *  ⚠️ EL MAR SALÍA CUBIERTO PERMANENTEMENTE, Y LLOVIENDO. La humedad del campo sobre el agua es
     *  ~1,0 (el horneado imprime `media 0.637 · max 1.000` contando el mar, contra los 0,286 de la
     *  tierra con los que se calibró `cloudCoverAt`). Con el fondo `0,73·H` el mar arranca en 0,7
     *  antes de que llegue ningún frente, y cualquier frente lo lleva por encima del umbral de
     *  lluvia. Pero el aire marino húmedo NO da cielo cerrado: da cúmulos de alisio dispersos y
     *  claros anticiclónicos enormes, con una media real de ~0,6 y mucha estructura. Lo que cierra
     *  el cielo sobre el mar son los frentes, y ya están. Así que sobre el agua la humedad que ve el
     *  CIELO se comprime a [0,28, 0,42]: fondo 0,20-0,31 y el resto lo ponen los frentes.
     *  ⚠️ Probé [0,40, 0,60] primero: media 0,644 pero **0,0 % de claros** (el fondo era 0,44 y por
     *  debajo de él no baja nadie) y lloviendo en el 32 % del mar. `sky_bake` lo mide.
     *  `water` es el complemento del `landMask` del terreno (0 tierra, 1 mar). */
    static float skyHumidity(float humidity, float water) {
        const float marine = 0.28f + 0.14f * glm::clamp(humidity, 0.0f, 1.0f);
        return glm::mix(glm::clamp(humidity, 0.0f, 1.0f), marine, glm::clamp(water, 0.0f, 1.0f));
    }

    /** @brief Desfase temporal de las capas altas respecto al frente, en segundos de clima.
     *
     *  Un cielo real tiene VARIAS capas porque no todas responden al mismo sitio del frente: el
     *  cirro llega cientos de km POR DELANTE (es el velo que anuncia el cambio) y el nivel medio
     *  (altocúmulo/altostrato) queda DETRÁS, en el aire que el frente deja. Sacar las tres capas de
     *  la misma cobertura daba una sola: con cielo poco cubierto sólo existía el cúmulo. Aquí el
     *  cirro se hornea con los frentes ADELANTADOS `kCirrusLeadS` y el nivel medio ATRASADO
     *  `kMidLagS`: con `kFrontRevolutionS` = 1800 s, 150 s son 30° de planeta ≈ 3300 km, que es
     *  el orden real de la antesala de cirros de una borrasca. */
    static constexpr double kCirrusLeadS = 150.0;
    static constexpr double kMidLagS     = 90.0;

    /** @brief Factor del viento a la altura de la nube respecto al de superficie. Las nubes van con
     *  el viento de su cota, no con el de los 10 m del suelo, y a 1-2 km sopla 2-3 veces más: con el
     *  de superficie (3,5 m/s en el spawn) un cúmulo de 1,4 km tardaba siete minutos en recorrer su
     *  propio ancho y se leía como quieto. */
    static constexpr float kCloudLevelWindMul = 2.6f;

    /**
     * @brief DENSIDAD DE NUBE en un punto 3D: cuánta nube hay a `altM` sobre el suelo. [0,1]
     *
     * Es la pieza que hace el clima tridimensional. `cloudCover` responde "¿hay nube sobre este
     * punto del planeta?"; esto responde "¿estoy DENTRO de ella?", que es otra pregunta y es la que
     * hace falta en cuanto el jugador puede volar: para el blanqueo al meterse en la nube, para ver
     * la capa de canto desde arriba, y para saber cuándo la has dejado por debajo.
     *
     * Es una función PURA de la muestra ya calculada (no vuelve a mirar los frentes), así que
     * evaluarla a lo largo de un rayo cuesta lo que cuesta la aritmética y nada más.
     *
     * Perfil vertical: 0 bajo la base, sube suave, macizo en el cuerpo, y se deshilacha hacia el
     * techo. Los bordes son suaves a propósito — un corte duro se lee como una pared de niebla, que
     * es exactamente lo que una nube no parece.
     *
     * @param w    muestra del clima en esa dirección (`sampleAt`).
     * @param altM altura sobre el SUELO (m), no sobre el nivel del mar.
     */
    static float cloudDensityAt(const WeatherSample& w, float altM);

    /** @brief ¿Está el punto DENTRO de la nube lo bastante para blanquear la vista? Umbral en un
     *  sitio, para que el render y la lógica no discrepen sobre qué es "estar en la nube". */
    static bool insideCloud(const WeatherSample& w, float altM) {
        return cloudDensityAt(w, altM) > 0.35f;
    }

    // ════════════════════════════════════════════════════════════════════════════════════════════
    //  TIEMPO ADVERSO — la severidad SALE DE LAS CONDICIONES, y el vórtice SALE DE LA SEVERIDAD
    // ════════════════════════════════════════════════════════════════════════════════════════════
    //
    // Es la misma regla que ya gobierna la lluvia («la precipitación sale de la cobertura, no de un
    // reloj aparte»), aplicada un piso más arriba. No hay un `spawnTornado()` ni una probabilidad
    // por minuto: un embudo existe porque en ese punto y a esa hora hay convección profunda, aire
    // húmedo y caliente y viento — y deja de existir cuando eso se acaba. Si se pudiera disparar
    // aparte volveríamos al bug de origen del fichero: dos relojes contando cosas distintas, y un
    // tornado con el cielo despejado.
    //
    // Y sigue siendo PURO: los vórtices no se guardan, se DERIVAN de (seed, tiempo, condiciones).
    // Ese es el requisito que hace que el DGS pueda validar un jugador arrastrado por un tornado sin
    // replicar ni un byte — el servidor evalúa el mismo campo. Un tornado con estado acumulado
    // habría que replicarlo, y entonces dejaría de poder validarse.

    /** @brief Escalones de tiempo adverso. Son los que puede leer el juego para decidir (avisos,
     *  refugio, cerrar el puerto, IA de los bichos): un número [0,1] no dice nada por sí solo. */
    enum class Severity : uint8_t {
        Calm = 0,   ///< nada que reportar
        Breezy,     ///< se mueve el follaje
        Windy,      ///< viento con el que ya cuesta andar de frente
        Gale,       ///< temporal: lluvia y viento de verdad
        Storm,      ///< tormenta con desarrollo vertical; aquí empieza a haber daño
        Severe      ///< condiciones de embudo: es donde `activeVortices` puede devolver algo
    };
    static const char* severityName(Severity s);

    /** @brief Índice [0,1] de cuán adverso es el tiempo en esa muestra.
     *
     *  Tres términos, y ninguno es decorativo:
     *   · la TORRE (grosor de la nube) manda, porque el desarrollo vertical es lo que distingue una
     *     tormenta de un día feo — es la misma cantidad que hace el yunque en `sampleAt`;
     *   · la PRECIPITACIÓN, que ya lleva dentro la cobertura y la humedad;
     *   · el VIENTO, que es lo único que se nota con el cielo medio despejado.
     *  Función pura de la muestra: no vuelve a mirar los frentes. */
    static float severityAt(const WeatherSample& w);
    static Severity severityClass(float s);

    /**
     * @brief POTENCIAL DE EMBUDO [0,1]: si en este punto podría formarse un tornado o una tromba.
     *
     * No es la severidad. Un temporal de viento en el mar del Norte es severo y no hace embudos; un
     * cumulonimbo de tarde de verano puede ser menos «severo» de viento medio y sí hacerlos. Lo que
     * hace falta es CONVECCIÓN PROFUNDA sobre aire cálido y húmedo. Por eso:
     *   · la torre entra con umbral alto (`kVortexMinTowerM`) y no gradualmente desde cero;
     *   · la temperatura entra como energía disponible (sobre hielo no hay tornados);
     *   · el viento entra POCO — un vendaval no hace un embudo, una tormenta quieta sí.
     *
     * @param overWater el punto está sobre agua. Cambia el régimen, no solo el nombre: la tromba
     *        marina se forma con MENOS energía (el mar entrega calor y humedad sin límite) y por eso
     *        son mucho más frecuentes que los tornados, pero es más pequeña y más débil.
     */
    static float vortexPotential(const WeatherSample& w, bool overWater);

    /** @brief Umbral de potencial a partir del cual hay embudo. Debajo puede haber tormenta seria y
     *  no pasa nada — que es lo que tiene que ocurrir casi siempre.
     *
     *  ⚠️ 0,42 NO ES UN NÚMERO REDONDO Y NO DEBE SERLO. El potencial máximo que alcanza el cinturón
     *  húmedo del mundo (H 0,70) es **0,49**, medido barriendo el planeta 120 horas de clima; el
     *  aire perfecto llega a 0,97 y la media del mundo a 0,00. O sea que el umbral tiene que caer
     *  entre 0,00 y 0,49 o el tornado no existe jamás fuera de un mundo de laboratorio — con 0,55,
     *  que era lo que había puesto a ojo, la medida daba CERO embudos en 20 horas de clima.
     *
     *  Y queda cerca del techo del cinturón a propósito: `drive = smoothstep(trigger, 1, pot)` sale
     *  casi 0 ahí, así que los embudos del mundo real nacen DÉBILES (los 34 m/s del suelo de la
     *  escala) y sólo el aire excepcional levanta un monstruo de 110 m/s. La fuerza la gradúa la
     *  energía disponible, que es como se gradúa de verdad. */
    static constexpr float kVortexTrigger  = 0.42f;
    /** @brief Ventana de torre (m) del potencial de embudo: umbral y saturación.
     *
     *  ⚠️ ESTABA EN [4500, 9500] Y LA MITAD ALTA NO EXISTE EN ESTE MUNDO. Un cumulonimbo terrestre
     *  llega a 12-15 km, así que 9500 parecía conservador. La nube de aquí no llega: `weather_severe`
     *  barre el planeta entero 120 horas de clima en cuatro regímenes e imprime la torre MÁXIMA de
     *  cada uno —
     *
     *      aire perfecto  (28 °C, H 0,90)  torre max 6397 m   ← el techo absoluto del modelo
     *      cinturon humedo(26 °C, H 0,70)  torre max 5145 m   ← el mejor aire que el mundo TIENE
     *      media del mundo(18 °C, H 0,29)  torre max 3131 m
     *      desierto       (30 °C, H 0,10)  torre max 2612 m
     *
     *  Con la ventana vieja el potencial topaba en 0,31 (aire perfecto) y 0,14 (cinturón húmedo)
     *  contra un disparo de 0,55: **cero embudos**, en todo el planeta y a cualquier hora. El sistema
     *  parecía roto y lo que estaba mal era el rango — el mismo error que este fichero ya documenta
     *  dos veces con la cobertura: calibrar contra un mundo que no es el que hay.
     *
     *  Los valores de ahora salen de esa tabla: el umbral deja fuera la media del mundo y el
     *  desierto por completo (3131 m no llega a 4300), y la saturación queda al alcance del cinturón
     *  húmedo. O sea que los tornados existen SÓLO donde hay energía, que es el comportamiento que
     *  se buscaba, y sale de la geografía del planeta en vez de una regla aparte.
     *
     *  ⚠️ Si la torre de `sampleAt` cambia, estos dos números hay que volver a MEDIRLOS con ese
     *  barrido, no estimarlos: el error anterior fue exactamente estimar. */
    static constexpr float kVortexMinTowerM  = 4300.0f;
    static constexpr float kVortexFullTowerM = 5800.0f;

    /**
     * @brief UN VÓRTICE: tornado o tromba marina. Es un CUERPO con eje, radio y vida, no un efecto.
     *
     * Se devuelve por valor y se recalcula cada frame a propósito: no hay identidad persistente que
     * mantener ni nada que replicar. `id` sirve para que el render pueda atarle partículas y sonido
     * entre frames sin que el sistema tenga que guardar estado.
     */
    struct Vortex {
        glm::dvec3 dir{0, 1, 0};   ///< dirección UNITARIA del eje desde el centro del planeta
        glm::dvec3 travel{0};      ///< unitario TANGENTE: hacia dónde se desplaza (para verlo venir)
        double coreRadiusM = 60.0; ///< radio del núcleo: donde el viento tangencial es MÁXIMO
        float  windMS  = 0.0f;     ///< viento tangencial máximo (en el núcleo)
        float  travelMS = 0.0f;    ///< con qué velocidad se desplaza el embudo
        float  spin    = 1.0f;     ///< +1 ciclónico / −1 anticiclónico
        float  age01   = 0.0f;     ///< 0 acaba de nacer, 1 se muere
        float  topM    = 1500.0f;  ///< techo de la columna = base de la nube madre
        bool   overWater = false;  ///< tromba marina en vez de tornado
        uint32_t cell  = 0;        ///< celda de la retícula que lo engendró
        uint32_t id    = 0;        ///< estable mientras vive; sirve para atar efectos entre frames
    };

    /**
     * @brief LADO DE LA CELDA de la retícula de embudos, en radianes de arco.
     *
     * ⚠️ ESTO SUSTITUYE A «N HUECOS POR FRENTE», Y EL MOTIVO ES UNA MEDIDA. Con 2 huecos por frente
     * (28 en todo el planeta) el sistema funcionaba y era inútil: `weather_severe` midió, desde un
     * punto fijo del cinturón húmedo y 20 horas de clima, **0,00 % del tiempo con un embudo a menos
     * de 300 km**. Y no es cuestión de subir el número: 28 huecos o 100, repartidos por una esfera
     * de 6371 km de radio, siguen sin caer nunca cerca de nadie. El problema no era la frecuencia,
     * era que la DENSIDAD no se podía elegir — dependía de cuántos frentes hubiera.
     *
     * Con retícula, la densidad ES el mando: un hueco por celda, y la celda se mide en kilómetros.
     * Y se consulta sólo alrededor de quien pregunta, así que el coste no depende del tamaño del
     * planeta sino del radio de búsqueda. Sigue siendo función pura de (seed, tiempo, posición): dos
     * clientes que miren la misma celda ven el mismo tornado, que es el requisito del DGS.
     *
     * 0,0055 rad ≈ 35 km de lado sobre la Tierra. La retícula es la del CUBO (misma familia que el
     * terreno), así que el lado real varía ~1,4× entre el centro de una cara y su esquina; es
     * irrelevante para sembrar y evita el amontonamiento en los polos de una rejilla lat/lon.
     */
    static constexpr double kVortexCellRad = 0.0055;

    /** @brief Techo de vórtices que puede devolver UNA consulta. No es un límite del mundo: es el
     *  tamaño del buffer que el llamador reserva. Con celdas de 35 km, un radio de búsqueda de
     *  100 km toca ~30 celdas y casi ninguna tiene embudo. */
    static constexpr int kMaxVortices = 16;

    /** @brief Radio de búsqueda que el motor usa por defecto (m): lo que hay que saber para JUGAR.
     *  Cubre de sobra el alcance físico del vórtice (unos 3 km) más lo que se ve venir desde lejos. */
    static constexpr double kVortexSearchM = 60000.0;

    /** @brief Vida de un embudo (s de simulación) y cada cuánto puede repetirse su hueco.
     *  Un tornado real dura entre minutos y media hora; 420 s con el reloj del clima
     *  (`kFrontRevolutionS` = 1800) es casi un cuarto de vuelta del frente: se ve venir, pasa y se
     *  va. `kVortexLifeS / kVortexCycleS` es el TECHO de cuánto tiempo puede estar ocupado un hueco
     *  —hoy el 3,9 %— y el suelo lo pone la condición, que casi siempre dice que no. Junto con el
     *  tamaño de celda, estos dos números SON la frecuencia con la que te pasa un tornado por encima;
     *  `weather_severe` la mide desde un punto fijo y la imprime. */
    static constexpr double kVortexLifeS  = 420.0;
    static constexpr double kVortexCycleS = 10800.0;

    /** @brief Sonda del terreno bajo un punto: el clima no conoce el suelo y no debe conocerlo.
     *  `sampleAt` ya recibe temperatura y humedad de fuera por el mismo motivo. */
    using FieldProbe = void (*)(const glm::dvec3& dir, void* user,
                                float& tempC, float& humidity, bool& overWater);

    /**
     * @brief Enumera los vórtices VIVOS ahora mismo CERCA de un punto. Devuelve cuántos ha escrito.
     *
     * No existe la consulta «todos los del planeta», y es deliberado: con retícula serían cientos de
     * miles de celdas por evaluar y ninguna de ellas importa a nadie. Quien quiera saber si hay un
     * tornado, pregunta por su alrededor.
     *
     * @param nearDir       dirección unitaria del observador
     * @param searchRadiusM cuánto alrededor mirar, en metros de arco
     * @param planetRadiusM radio del planeta (convierte arco a metros)
     * @param probe         condiciones del campo en la dirección dada — es lo que hace que el embudo
     *                      salga donde hay energía y no donde toque por sorteo.
     */
    int activeVortices(const glm::dvec3& nearDir, double searchRadiusM, double planetRadiusM,
                       Vortex* out, int maxOut, FieldProbe probe, void* user) const;
    int activeVortices(const glm::dvec3& nearDir, double searchRadiusM, double planetRadiusM,
                       Vortex* out, int maxOut, float tempC, float humidity, bool overWater) const;

    /**
     * @brief CAMPO DE VELOCIDADES del vórtice en un punto, en m/s y en el marco LOCAL DEL PLANETA.
     *
     * Esto es «con físicas»: no es una animación ni un empujón escalar, es un campo que cualquiera
     * puede consultar —el jugador, un prop suelto, la lluvia, el agua del mar bajo la tromba— y del
     * que se saca fuerza con `½·ρ·Cd·A·|v|²` del lado de quien la sufre. Tres componentes, y las
     * tres se notan:
     *   · TANGENCIAL (Rankine): sube lineal hasta el núcleo y cae como 1/r fuera. Es el perfil
     *     medido de un vórtice real y es lo que hace que el daño tenga un anillo, no un disco.
     *   · ENTRADA RADIAL cerca del suelo: es lo que ARRASTRA las cosas hacia dentro. Sin ella, todo
     *     orbitaría para siempre a la misma distancia y nada sería succionado.
     *   · ASCENDENTE en el núcleo: es lo que LEVANTA. Y es la que hace que una tromba marina
     *     levante agua, que es literalmente lo que la hace visible.
     *
     * @param dir  dirección unitaria del punto consultado (marco local del planeta)
     * @param planetRadiusM radio del planeta: convierte el ángulo al eje en METROS
     * @param altM altura sobre el suelo (m)
     */
    static glm::dvec3 vortexWindAt(const Vortex& v, const glm::dvec3& dir,
                                   double planetRadiusM, float altM);

    /** @brief Radio (en núcleos) más allá del cual `vortexWindAt` devuelve 0 exacto. El perfil 1/r
     *  no llega nunca a cero por sí solo, y sin corte un tornado movería una hoja en otro
     *  continente: 14 núcleos es donde el tangencial ya ha caído por debajo del 7 % del pico. */
    static constexpr double kVortexReachCores = 14.0;

private:
    /** @brief Un frente: un casquete que gira alrededor de su eje a velocidad constante. */
    struct Front {
        glm::dvec3 axis{0, 1, 0};   ///< eje de giro (unitario)
        glm::dvec3 start{1, 0, 0};  ///< centro en t=0 (unitario, perpendicular-ish al eje)
        double     omega = 0.0;     ///< rad/s (con signo: los frentes no van todos en el mismo sentido)
        double     radius = 0.3;    ///< radio ANGULAR del casquete (rad)
        float      strength = 1.0f; ///< [0,1] cuánto tapa en su núcleo
    };

    /** @brief Centro del frente i en el instante actual (girado sobre su eje). */
    glm::dvec3 frontCenter(int i) const;

    Front    m_fronts[kFronts]{};
    uint32_t m_seed = 0;
    double   m_time = 0.0;
    double   m_timeScale = 1.0;
    bool     m_configured = false;
};

} // namespace Haruka
