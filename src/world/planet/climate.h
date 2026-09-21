#pragma once
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>   // glm::pi — llegaba de rebote por el cube_sphere.h borrado

namespace Haruka { namespace Planet {

// ===========================================================================
// Climate layer — optional
// Produces temperature and humidity maps based on latitude, elevation,
// axial tilt, and orographic effects.
// ===========================================================================

struct ClimateConfig {
    double axialTiltDeg = 23.44;   // Earth-like
    double seaLevelTemp = 15.0;    // °C at sea level equator
    double lapseRate    = 6.5;     // °C per km of elevation
    double poleTemp     = -30.0;   // °C at pole sea level

    // Wind & humidity
    double prevailingWindSpeed = 10.0; // m/s
    double moistureFalloff     = 0.3;  // per radian from ocean
    unsigned seed = 42;
};

struct ClimateOutput {
    // Query at any unit direction + elevation (km)
    double temperature(const glm::dvec3& dir, double elevationKm, bool isOcean) const;
    double humidity(const glm::dvec3& dir, double elevationKm, bool isOcean) const; // 0-1
    /// Humedad del SUELO (ignora que esté bajo el agua). La que debe leer la selección de material.
    double groundHumidity(const glm::dvec3& dir, double elevationKm) const;
    double precipitation(const glm::dvec3& dir, double elevationKm, bool isOcean) const; // m/year
};

// ⚠️ ESTE clima es el que HORNEA el mapa de biomas (BiomeClassifyNode). El que lee el shader por
// píxel es OTRO: el campo de `PlanetFields::computeClimate` (SSBO binding 10). Los dos tienen que
// describir el MISMO planeta o el color del terreno contradice a la humedad con la que se elige la
// textura y con la que se colocan los props. Los perfiles de abajo son los de `PlanetFields`, con
// las mismas constantes y por las mismas razones — ver el comentario largo de aquel fichero.
inline double ClimateOutput::temperature(const glm::dvec3& dir, double elevationKm, bool) const {
    const double lat    = std::asin(glm::clamp(std::abs(dir.y), 0.0, 1.0));
    const double latDeg = lat * 180.0 / glm::pi<double>();

    // Perfil zonal en GRADOS (no en |y|, que es el SENO de la latitud): ~27 °C en el ecuador,
    // ~10 °C a 53°, ~−21 °C en el polo. La rampa LINEAL anterior daba 15 °C de máximo en TODO el
    // planeta — 12 °C por debajo del ecuador real—, así que ninguna fila cálida podía activarse y
    // el mapa de biomas no tenía ni sabana ni selva en ninguna latitud.
    double t = 27.0 - 0.0060 * latDeg * latDeg;

    t -= std::max(0.0, elevationKm) * 6.5;   // gradiente vertical real
    return t;
}

/**
 * @brief Humedad del SUELO, ignorando que esté bajo el agua.
 *
 * ⚠️ EXISTE PORQUE `humidity()` DEVUELVE 1.0 SOBRE OCÉANO Y ESO PINTABA EL PLANETA DE VERDE.
 *
 * Ese 1.0 es correcto como CLIMA —el mar es la fuente de humedad, y así alimenta la precipitación de
 * la tierra de al lado— pero no como "aquí crece hierba". La selección de material lo leía tal cual,
 * así que el fondo oceánico entero ganaba el material húmedo: medido en su día, **~80 % del disco
 * visible clasificado como `green`**. De ahí que el planeta se viera de un verde uniforme desde
 * órbita, y no de la paleta de biomas — que sí tiene 58 colores y RMS 0,27 de dispersión.
 *
 * El suelo submarino sigue siendo SUELO: se clasifica con la misma fórmula que la tierra emergida,
 * por latitud y temperatura. Lo que hay encima lo dibuja el agua, no el material.
 */
inline double ClimateOutput::groundHumidity(const glm::dvec3& dir, double elevationKm) const {
    return humidity(dir, elevationKm, false);
}

inline double ClimateOutput::humidity(const glm::dvec3& dir, double elevationKm, bool isOcean) const {
    if (isOcean) return 1.0;                 // el mar ES la fuente de humedad (ver `groundHumidity`)

    const double lat    = std::asin(glm::clamp(std::abs(dir.y), 0.0, 1.0));
    const double latDeg = lat * 180.0 / glm::pi<double>();

    // PERFIL ZONAL por celdas de circulación, no una constante. La versión anterior daba 0.3 fijo
    // sobre tierra, que tras modular por temperatura quedaba en 0.15–0.24 en TODO el planeta: por
    // debajo de `forestEdge0` (0.46), así que bosque y selva eran INALCANZABLES sobre tierra y toda
    // la superficie emergida salía desierto o estepa. De ahí que el planeta se viera liso.
    //   · ITCZ (0°)         húmedo  → selva ecuatorial
    //   · subtropical (25°) seco    → el cinturón de desiertos
    //   · templado (52°)    húmedo  → bosque
    //   · polar             seco    → desierto frío
    auto gauss = [](double x, double mu, double sigma) {
        const double d = (x - mu) / sigma;
        return std::exp(-d * d);
    };
    double h = 0.30
             + 0.62 * gauss(latDeg,  0.0, 12.0)
             - 0.24 * gauss(latDeg, 25.0, 12.0)
             + 0.34 * gauss(latDeg, 52.0, 14.0);

    // El aire frío retiene menos vapor: esto es lo que seca los polos sin una banda propia.
    const double t = temperature(dir, elevationKm, isOcean);
    const double tempFactor = glm::clamp((t + 10.0) / 40.0, 0.0, 1.0);
    h *= 0.5 + 0.5 * tempFactor;

    // Por encima de 2 km el aire ya ha descargado: sombra de lluvia de altura (la orográfica de
    // verdad —trazar contra el viento hasta el mar— necesita la retícula de elevación y vive en
    // `PlanetFields`; aquí solo se aproxima la parte que depende de la cota).
    if (elevationKm > 2.0)
        h *= std::max(0.2, 1.0 - (elevationKm - 2.0) * 0.1);

    return glm::clamp(h, 0.0, 1.0);
}

inline double ClimateOutput::precipitation(const glm::dvec3& dir, double elevationKm, bool isOcean) const {
    double h = humidity(dir, elevationKm, isOcean);

    // Base precipitation from humidity
    double p = h * 1.5; // m/year max

    // Orographic enhancement: windward slopes get more rain
    if (elevationKm > 0.5 && elevationKm < 3.0) {
        p += elevationKm * 0.3;
    }

    // Rain shadow above 3km
    if (elevationKm > 3.0) {
        p *= std::max(0.1, 1.0 - (elevationKm - 3.0) * 0.2);
    }

    return std::max(0.0, p);
}

}} // namespace Haruka::Planet
