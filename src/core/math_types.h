/**
 * @file math_types.h
 * @brief Engine-wide math type aliases and astrophysical unit constants.
 *
 * All world-space positions use double precision (`WorldPos = dvec3`) to
 * avoid floating-point precision loss at astronomical distances. Local/render
 * positions use single precision (`LocalPos = vec3`).
 *
 * The `Units` namespace provides named constants and converters so that
 * distances expressed in km remain readable at every scale (planetary → galactic).
 */
#ifndef MATH_TYPES_H
#define MATH_TYPES_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Haruka {
    /** @brief World-space position type used across astronomical systems. */
    using WorldPos = glm::dvec3; 
    /** @brief Local-space position type used for render/gameplay transforms. */
    using LocalPos = glm::vec3;
    
    /** @brief Quaternion rotation type used for double-precision orientation. */
    using Rotation = glm::dquat;

    /** @brief Unit and scale helpers for astrophysical distances. */
    namespace Units
    {
        /** @brief Base unit: one meter. All world-space coordinates are in metres. */
        constexpr double METER = 1.0;
        constexpr double KM    = 1000.0 * METER;

        /** @name Large-scale distance helpers */
        ///@{
        constexpr double MEGAMETER = 1000.0 * KM; // 10⁶ m
        constexpr double GIGAMETER = 1000000.0 * KM; // 10⁶ km
        constexpr double TERAMETER = 1000000000.0 * KM; // 10⁹ km
        constexpr double PETAMETER = 1000000000000.0 * KM; // 10¹² km
        ///@}

        /** @name Celestial scale helpers */
        ///@{
        constexpr double PLANETARY_RADIUS_SMALL = 1000.0 * KM;
        constexpr double PLANETARY_RADIUS_MEDIUM = 6000.0 * KM;
        constexpr double PLANETARY_RADIUS_LARGE = 70000.0 * KM;

        constexpr double ORBITAL_DISTANCE_CLOSE = 100.0 * MEGAMETER;
        constexpr double ORBITAL_DISTANCE_MEDIUM = 150.0 * GIGAMETER;
        constexpr double ORBITAL_DISTANCE_LARGE = 5.0 * TERAMETER;

        constexpr double STAR_RADIUS_SMALL = 500000.0 * KM;
        constexpr double STAR_RADIUS_MEDIUM = 700000.0 * KM;
        constexpr double STAR_RADIUS_LARGE = 1500000.0 * KM;
        ///@}

        /** @name Galactic scale helpers */
        ///@{
        constexpr double LIGHT_SPEED = 299792.458 * KM;
        constexpr double LIGHT_YEAR = 9.460730e12 * KM;
        constexpr double PARSEC = 3.0857e13 * KM;

        constexpr double GALACTIC_RADIUS = 50000.0 * LIGHT_YEAR; 
        constexpr double INTERGALACTIC_DISTANCE = 2.5e6 * LIGHT_YEAR;
        ///@}

        /** @brief Converts metres to render units. */
        inline double mToRender(double m) { return m / METER; }

        /** @brief Converts kilometres to render units. */
        inline double kmToRender(double km) { return km * KM / METER; }

        inline glm::dvec3 kmToRender(const glm::dvec3& kmVec) { return kmVec * KM / METER; }
        inline glm::vec3  kmToRender(const glm::vec3&  kmVec) { return kmVec * static_cast<float>(KM / METER); }
    }
};
#endif