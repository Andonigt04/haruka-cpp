#pragma once
#include <string>
#include <glm/glm.hpp>

namespace Haruka { namespace Planet {

// ===========================================================================
// Biomes layer — optional
// Combines elevation, temperature, and precipitation into biome types.
// ===========================================================================

enum class Biome : uint8_t {
    Ocean,
    DeepOcean,
    Beach,
    Tundra,
    Taiga,
    BorealForest,
    TemperateForest,
    TemperateRainforest,
    TropicalRainforest,
    TropicalSeasonalForest,
    Savanna,
    Grassland,
    Shrubland,
    Desert,
    Mediterranean,
    AlpineTundra,
    Snow,
    Ice,
    Cliff,      ///< de PENDIENTE, no de clima: el mismo umbral (0,55 rad) que el material `rock`
    COUNT
};

/**
 * @brief Clave CORTA del bioma para el `when` de las capas de props (`biome == taiga`), y para el
 *  log. Minúsculas, sin espacios, en español donde el nombre inglés no es el de uso: es lo que se
 *  escribe en el JSON de la escena, así que tiene que poder teclearse sin comillas.
 */
inline const char* biomeKey(Biome b) {
    switch (b) {
        case Biome::Ocean:                  return "ocean";
        case Biome::DeepOcean:              return "ocean_profundo";
        case Biome::Beach:                  return "playa";
        case Biome::Tundra:                 return "tundra";
        case Biome::Taiga:                  return "taiga";
        case Biome::BorealForest:           return "boreal";
        case Biome::TemperateForest:        return "bosque";
        case Biome::TemperateRainforest:    return "bosque_lluvioso";
        case Biome::TropicalRainforest:     return "selva";
        case Biome::TropicalSeasonalForest: return "selva_estacional";
        case Biome::Savanna:                return "sabana";
        case Biome::Grassland:              return "pradera";
        case Biome::Shrubland:              return "matorral";
        case Biome::Desert:                 return "desierto";
        case Biome::Mediterranean:          return "mediterraneo";
        case Biome::AlpineTundra:           return "alpino";
        case Biome::Snow:                   return "nieve";
        case Biome::Ice:                    return "hielo";
        case Biome::Cliff:                  return "acantilado";
        default:                            return "?";
    }
}

/**
 * @brief RUGOSIDAD DEL LECHO para el agua que corre por él: la `n` de Manning (s·m^-1/3) del suelo
 *        de ese bioma. Es lo que hace que la fricción del agua interior sea DEL SITIO y no un número
 *        único: sobre roca lisa o hielo una lámina resbala; sobre un bosque, con hojarasca y raíces,
 *        se para en seguida.
 *
 * Valores de las tablas de hidráulica de canales (Chow, 1959; USGS): hormigón/hielo 0,012, arena
 * 0,022, grava 0,025, hierba corta 0,030, pradera alta 0,035, matorral 0,05–0,08, bosque con
 * sotobosque 0,10–0,16. La fricción resultante crece con la velocidad y cae con el calado
 * (`g·n²·|u|/h^(4/3)`), así que una lámina fina sobre el mismo suelo se frena antes que una gruesa —
 * ver `ShallowWaterSim::substep`. `test_shallow_water_friction` mide las dos cosas.
 */
inline float manningForBiome(Biome b) {
    switch (b) {
        case Biome::Ice:                    return 0.012f;   // hielo: liso
        case Biome::Ocean:
        case Biome::DeepOcean:              return 0.020f;   // fondo marino: arena/limo
        case Biome::Beach:
        case Biome::Desert:                 return 0.022f;   // arena
        case Biome::Cliff:                  return 0.028f;   // roca desnuda, irregular
        case Biome::Snow:                   return 0.030f;
        case Biome::Grassland:
        case Biome::Savanna:                return 0.035f;   // hierba
        case Biome::Tundra:
        case Biome::AlpineTundra:           return 0.040f;   // musgo, piedra suelta
        case Biome::Shrubland:
        case Biome::Mediterranean:          return 0.060f;   // matorral
        case Biome::Taiga:
        case Biome::BorealForest:
        case Biome::TemperateForest:
        case Biome::TemperateRainforest:
        case Biome::TropicalSeasonalForest:
        case Biome::TropicalRainforest:     return 0.120f;   // bosque con sotobosque
        default:                            return 0.030f;
    }
}

/** @brief Inversa de `biomeKey`: `Biome::COUNT` si la clave no existe (el validador la usa para
 *  avisar de un `biome == bosqe` con errata antes de que el planeta la ignore en silencio). */
inline Biome biomeFromKey(const std::string& key) {
    for (int i = 0; i < (int)Biome::COUNT; ++i)
        if (key == biomeKey((Biome)i)) return (Biome)i;
    return Biome::COUNT;
}

/** @brief El bioma de BOSQUE que corresponde a una temperatura: lo que se usa cuando un mapa de
 *  distribución pintado dice "aquí hay bosque" y el clima dice otra cosa (el perfil zonal de humedad
 *  deja Kyushu en matorral con 0,12). Taiga bajo 11 °C, bosque templado hasta 18, selva estacional
 *  por encima — los mismos cortes que `classifyBiome`. */
inline Biome forestBiomeForTemp(float tempC) {
    if (tempC <= 11.0f) return Biome::Taiga;
    if (tempC <= 18.0f) return Biome::TemperateForest;
    return Biome::TropicalSeasonalForest;
}

/** @brief ¿Puede un mapa pintado convertir este bioma en bosque? El mar, el acantilado y el hielo
 *  no: ahí el mapa no manda. */
inline bool biomeAcceptsForestMap(Biome b) {
    return b != Biome::Ocean && b != Biome::DeepOcean && b != Biome::Cliff &&
           b != Biome::Snow && b != Biome::Ice;
}

/** @brief Lo que el bioma de un PUNTO necesita: lo mismo que ya muestrea el scatter de props por
 *  celda, así clasificar no cuesta ni un muestreo más. */
struct BiomeSample {
    float tempC    = 15.0f;   ///< temperatura media (°C), la del `FieldSample`
    float humidity = 0.5f;    ///< humedad [0,1], la del `FieldSample` (NO precipitación m/año)
    float elevM    = 0.0f;    ///< cota sobre el nivel del mar (m)
    float slopeRad = 0.0f;    ///< pendiente local (rad)
};

/**
 * @brief Bioma de un punto a partir de HUMEDAD (no precipitación), pendiente y cota.
 *
 * `BiomesOutput::classify` es el clasificador del albedo procedural orbital y va por precipitación
 * en m/año; los props y los materiales del terreno no ven eso: ven `FieldSample::humidity` [0,1].
 * Dos clasificadores con umbrales sueltos dirían "taiga" el uno y "pradera" el otro en el mismo
 * sitio, así que ÉSTE toma sus cortes de `TerrainMaterialTable::defaults()`: humedad 0,42 (dry|green,
 * tundra|taiga), temperatura 11 °C (cálido|frío) y −12 °C (hielo), pendiente 0,55 rad (rock). Un
 * árbol de taiga cae así donde el suelo se pinta de taiga, no a dos texeles.
 *
 * Playa: cota ≤ 6 m y llano. Es una banda de cota, no de distancia a la costa (no hay mapa de
 * distancia a la costa): una llanura interior a 3 m también sale "playa". Se documenta, no se tapa.
 */
inline Biome classifyBiome(const BiomeSample& s) {
    if (s.elevM < 0.0f)      return s.elevM < -4000.0f ? Biome::DeepOcean : Biome::Ocean;
    if (s.slopeRad > 0.55f)  return Biome::Cliff;
    if (s.tempC < -12.0f)    return s.elevM > 2000.0f ? Biome::Ice : Biome::Snow;
    if (s.elevM <= 6.0f && s.slopeRad < 0.25f) return Biome::Beach;
    if (s.elevM > 3000.0f && s.tempC < 5.0f)   return Biome::AlpineTundra;
    if (s.tempC < 0.0f)      return Biome::Tundra;

    const float h = s.humidity, t = s.tempC;
    if (h < 0.25f) {                                   // árido
        if (t > 15.0f) return Biome::Desert;
        if (t > 5.0f)  return Biome::Shrubland;
        return Biome::Tundra;
    }
    if (h > 0.75f) {                                   // muy húmedo
        if (t > 18.0f) return Biome::TropicalRainforest;
        if (t > 11.0f) return Biome::TemperateRainforest;
        return Biome::BorealForest;
    }
    if (h >= 0.42f) {                                  // húmedo: el corte green/taiga del terreno
        if (t > 18.0f) return Biome::TropicalSeasonalForest;
        if (t > 11.0f) return Biome::TemperateForest;
        return Biome::Taiga;
    }
    if (h > 0.32f) {                                   // semiárido
        if (t > 18.0f) return Biome::Savanna;
        if (t > 11.0f) return Biome::Grassland;
        return Biome::Tundra;
    }
    return t > 15.0f ? Biome::Mediterranean : Biome::Shrubland;
}

inline const char* biomeName(Biome b) {
    switch (b) {
        case Biome::Ocean:                  return "Ocean";
        case Biome::DeepOcean:              return "Deep Ocean";
        case Biome::Beach:                  return "Beach";
        case Biome::Tundra:                 return "Tundra";
        case Biome::Taiga:                  return "Taiga";
        case Biome::BorealForest:           return "Boreal Forest";
        case Biome::TemperateForest:        return "Temperate Forest";
        case Biome::TemperateRainforest:    return "Temperate Rainforest";
        case Biome::TropicalRainforest:     return "Tropical Rainforest";
        case Biome::TropicalSeasonalForest: return "Tropical Seasonal Forest";
        case Biome::Savanna:                return "Savanna";
        case Biome::Grassland:              return "Grassland";
        case Biome::Shrubland:              return "Shrubland";
        case Biome::Desert:                 return "Desert";
        case Biome::Mediterranean:          return "Mediterranean";
        case Biome::AlpineTundra:           return "Alpine Tundra";
        case Biome::Snow:                   return "Snow";
        case Biome::Ice:                    return "Ice";
        case Biome::Cliff:                  return "Cliff";
        default: return "Unknown";
    }
}

struct BiomesConfig {
    double oceanLevel = 0.0; // km — elevation below this is ocean
};

struct BiomesOutput {
    Biome classify(double elevationKm, double temperature, double precipitation,
                   bool isOcean, double latitude) const;
    glm::vec3 color(Biome b) const; // RGB color for visualization
};

inline Biome BiomesOutput::classify(double elevationKm, double temperature,
                                     double precipitation, bool isOcean,
                                     double latitude) const {
    if (isOcean) {
        if (elevationKm < -4.0) return Biome::DeepOcean;
        return Biome::Ocean;
    }

    // Ice & snow
    if (temperature < -15.0) {
        if (elevationKm > 2.0) return Biome::Ice;
        return Biome::Snow;
    }

    // Alpine
    if (elevationKm > 3.0 && temperature < 5.0) return Biome::AlpineTundra;

    // Tundra
    if (temperature < 0.0) return Biome::Tundra;

    // Desert (hot or cold)
    if (precipitation < 0.25) {
        if (temperature > 15.0) return Biome::Desert;
        if (temperature > 5.0) return Biome::Shrubland;
        return Biome::Tundra;
    }

    // Forest
    if (precipitation > 1.5) {
        if (temperature > 18.0) return Biome::TropicalRainforest;
        if (temperature > 10.0) return Biome::TemperateRainforest;
        return Biome::BorealForest;
    }

    if (precipitation > 0.8) {
        if (temperature > 18.0) return Biome::TropicalSeasonalForest;
        if (temperature > 5.0)  return Biome::TemperateForest;
        return Biome::Taiga;
    }

    // Grass/shrub
    if (precipitation > 0.4) {
        if (temperature > 18.0) return Biome::Savanna;
        if (temperature > 5.0)  return Biome::Grassland;
        return Biome::Tundra;
    }

    if (temperature > 15.0) return Biome::Mediterranean;
    return Biome::Shrubland;
}

inline glm::vec3 BiomesOutput::color(Biome b) const {
    switch (b) {
        case Biome::Ocean:                  return {0.0f, 0.2f, 0.5f};
        case Biome::DeepOcean:              return {0.0f, 0.05f, 0.2f};
        case Biome::Beach:                  return {0.8f, 0.7f, 0.4f};
        case Biome::Tundra:                 return {0.5f, 0.5f, 0.4f};
        case Biome::Taiga:                  return {0.2f, 0.4f, 0.2f};
        case Biome::BorealForest:           return {0.15f, 0.35f, 0.15f};
        case Biome::TemperateForest:        return {0.2f, 0.5f, 0.2f};
        case Biome::TemperateRainforest:    return {0.1f, 0.4f, 0.15f};
        case Biome::TropicalRainforest:     return {0.05f, 0.45f, 0.1f};
        case Biome::TropicalSeasonalForest: return {0.2f, 0.5f, 0.15f};
        case Biome::Savanna:                return {0.6f, 0.5f, 0.2f};
        case Biome::Grassland:              return {0.4f, 0.6f, 0.2f};
        case Biome::Shrubland:              return {0.5f, 0.4f, 0.2f};
        case Biome::Desert:                 return {0.8f, 0.7f, 0.3f};
        case Biome::Mediterranean:          return {0.4f, 0.6f, 0.3f};
        case Biome::AlpineTundra:           return {0.4f, 0.35f, 0.3f};
        case Biome::Snow:                   return {0.9f, 0.9f, 1.0f};
        case Biome::Ice:                    return {0.8f, 0.85f, 1.0f};
        case Biome::Cliff:                  return {0.45f, 0.42f, 0.40f};
        default: return {1, 0, 1};
    }
}

}} // namespace Haruka::Planet
