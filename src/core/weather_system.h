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

    /** @brief Cobertura a partir de la cual el frente descarga. Debajo hay nube pero no llueve —
     *  que es justo la mitad de "la lluvia cae de las nubes": nube sin lluvia SÍ existe. */
    static constexpr float kPrecipCover = 0.62f;

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
