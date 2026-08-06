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
    COUNT
};

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
        default: return {1, 0, 1};
    }
}

}} // namespace Haruka::Planet
