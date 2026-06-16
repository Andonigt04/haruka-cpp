#pragma once

#include "tools/math_types.h"
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include "tools/planetary_types.h" // El World posee al PlanetaryTypes
#include "tools/object_types.h" // Para ObjectType en CelestialBody

namespace Haruka {

    class PlanetarySystem;
    class SceneManager;

    /**
     * @brief Representa una entidad macroscópica en el vacío del espacio.
     */
    struct CelestialBody {
        std::string name;
        Haruka::ObjectType type;
        WorldPos worldPos;     // Posición en double (km)
        glm::vec3 velocity;    // km/s
        double mass = 0.0;     // kg
        float radius;          // km
        glm::vec3 color;
        
        // Propiedades de renderizado calculadas cada frame
        LocalPos localPos;     // Posición relativa a la cámara (float)
    };

    /**
     * @brief El WorldSystem gestiona la escala del universo y el Floating Origin.
     */
    class WorldSystem {
    public:
        WorldSystem();
        ~WorldSystem();

        void init();
        
        /**
         * @brief Actualiza todo el universo.
         */
        void update(double dt, const glm::dvec3& cameraPos);

        /**
         * @brief Convierte una posición global (double) a una relativa a la cámara (float)
         * para evitar que los modelos tiemblen en la GPU.
         */
        glm::vec3 toLocal(const glm::dvec3& worldPos) const;

        /**
         * @brief Mueve el centro del universo matemático a la posición de la cámara.
         */
        void shiftOrigin(const glm::dvec3& newOrigin);

        // Getters de los sistemas especializados
        PlanetarySystem& getPlanetarySystem() { return *m_planetarySystem; }
        const std::vector<CelestialBody>& getBodies() const { return m_bodies; }
        std::vector<CelestialBody>& getBodies() { return m_bodies; }

        /** Reads CelestialBody/Star objects from the scene into m_bodies. */
        void syncFromScene(const SceneManager& scene);

        /**
         * Returns the combined normalized light direction summed from all stars,
         * weighted by 1/distance². observerPos uses double precision.
         * Falls back to (0,1,0) if no stars are registered.
         */
        glm::vec3 getDominantLightDirection(const glm::dvec3& observerPos) const;

        /**
         * Returns the blended light color from all stars weighted by 1/distance².
         * Falls back to (1,1,1) if no stars are registered.
         */
        glm::vec3 getDominantLightColor(const glm::dvec3& observerPos) const;

        /** @brief Escala de amplitud del oleaje por MAREA: marea viva (~1.35) cuando
         *  Sol y Luna están alineados (luna nueva/llena), marea muerta (~0.55) en
         *  cuadratura. 1.0 si no hay Luna. El render del agua lo pasa a u_windStrength. */
        float getTideFactor() const { return m_tideFactor; }

        /** @brief Luz de luna para el observador dado. outDir = dirección HACIA la Luna,
         *  outIntensity = brillo según la FASE (llena≈máx, nueva≈0). Devuelve false si
         *  no hay Luna (entonces no hay 2ª luz). */
        bool getMoonLight(const glm::dvec3& observer, glm::vec3& outDir, float& outIntensity) const;

        /** @brief Fija el planeta ACTIVO (centro+radio) que usan atmósfera/día-noche/
         *  luna/viento. Lo pasa Application desde el PlanetarySystem REAL (el de
         *  WorldSystem está vacío). Sin esto, todo lo celeste cae a sus fallbacks. */
        void setActivePlanet(const glm::dvec3& center, double radius) {
            m_planetCenter = center; m_planetRadius = radius; m_hasActivePlanet = true;
        }
        /** @brief Avanza la mecánica celeste un frame: órbita del Sol (día/noche),
         *  órbita de la Luna, marea y tiempo atmosférico. Lo llama el render. */
        void advanceCelestial(double dt);

        // --- Atmósfera ---
        /** @brief Elevación solar: dot(arriba_local, dirección_al_Sol). 1=mediodía,
         *  0=horizonte, <0=noche. 1.0 si no hay planeta. */
        float getSunElevation(const glm::dvec3& observer) const;
        /** @brief Color de cielo/clear por elevación solar y altitud (azul de día →
         *  cálido al amanecer/atardecer → oscuro de noche → negro en el espacio). */
        glm::vec3 getSkyColor(const glm::dvec3& observer) const;
        /** @brief Viento atmosférico (m/s) en una posición: brisa global tangente a la
         *  superficie con dirección que gira lento + ráfagas. Cero lejos del planeta. */
        glm::vec3 getWind(const glm::dvec3& worldPos) const;

    private:
        // Avanza el ciclo día/noche (orbita el Sol alrededor del planeta).
        void updateDayNight(double dt);
        // Orbita la Luna (objeto de escena "Moon") alrededor del planeta.
        void updateMoon(double dt);

        // Sistemas coordinados por el Mundo
        std::unique_ptr<PlanetarySystem> m_planetarySystem;
        
        // El "Floating Origin" (Punto 0,0,0 real en el espacio)
        glm::dvec3 m_worldOrigin;
        
        // Lista de entidades macroscópicas (estrellas, estaciones, planetas)
        std::vector<CelestialBody> m_bodies;

        // --- Mecánica celeste (ciclo día/noche) ---
        // Orbitamos el SOL alrededor del planeta (equivale a girar el planeta sobre
        // su eje, pero sin mover el terreno ni al jugador). La dirección de luz sigue
        // al Sol → amanece y anochece. El Sol visible (objeto de escena) se mueve con
        // él. m_scene permite escribir la posición del objeto de escena.
        SceneManager* m_scene = nullptr;
        bool          m_celestialInit = false;
        glm::dvec3    m_sunOffset0{0.0}; // offset Sol→planeta inicial (se rota en Y)
        double        m_dayAngle = 0.0;  // ángulo del día acumulado (rad)

        // --- Órbita de la Luna (objeto de escena "Moon", la crea el juego) ---
        bool          m_moonInit = false;
        glm::dvec3    m_moonOffset0{0.0};
        double        m_moonAngle = 0.0;

        // Marea actual (escala de amplitud del oleaje) por alineación Sol–Luna.
        float         m_tideFactor = 1.0f;
        void updateTide(); // recalcula m_tideFactor (tras mover Sol y Luna)

        // Tiempo de la atmósfera (s) para la deriva/ráfagas del viento.
        double        m_atmoTime = 0.0;

        // Planeta ACTIVO real (lo fija Application; el m_planetarySystem propio está
        // vacío). Lo usan atmósfera/día-noche/luna/viento/marea.
        glm::dvec3    m_planetCenter{0.0};
        double        m_planetRadius = 0.0;
        bool          m_hasActivePlanet = false;
    };

}