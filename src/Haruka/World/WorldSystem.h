#pragma once
/**
 * @file WorldSystem.h
 * @brief `WorldSystem`: la escala del universo y el origen flotante.
 *
 * Un `float` no llega. A la distancia de una órbita, su resolución se mide en
 * metros y los modelos tiemblan. Por eso el motor mantiene dos mundos separados y
 * este fichero es la frontera:
 *
 * - Arriba, **`double` y kilómetros**: cada @ref Haruka::World::CelestialBody guarda su
 *   `WorldPos`, y ahí se simula.
 * - Abajo, **`float` y metros relativos a la cámara**: lo único que sube a la GPU.
 *
 * `shiftOrigin()` recentra el universo en la cámara y `toLocal()` es la única
 * conversión sancionada entre los dos. Cualquier `glm::vec3` que nazca de restar
 * dos posiciones globales a mano se salta esta frontera y trae de vuelta el
 * temblor.
 */

#include "Haruka/World/Planet/SkyEphemeris.h"
#include "Haruka/Tools/MathTypes.h"
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include "Haruka/Tools/PlanetaryTypes.h" // El World posee al PlanetaryTypes
#include "Haruka/Tools/ObjectTypes.h" // Para ObjectType en CelestialBody

namespace Haruka::World::Planet { class PlanetarySystem; }
namespace Haruka::Core::Scene { class SceneManager; }


namespace Haruka::World {
    /**
     * @brief Representa una entidad macroscópica en el vacío del espacio.
     */
    struct CelestialBody {
        std::string name;
        Tools::ObjectType type;
        Tools::WorldPos worldPos;     // Posición en double (km)
        glm::vec3 velocity;    // km/s
        double mass = 0.0;     // kg
        float radius;          // km
        glm::vec3 color;
        
        // Propiedades de renderizado calculadas cada frame
        Tools::LocalPos localPos;     // Posición relativa a la cámara (float)
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
        Planet::PlanetarySystem& getPlanetarySystem() { return *m_planetarySystem; }
        const std::vector<CelestialBody>& getBodies() const { return m_bodies; }
        std::vector<CelestialBody>& getBodies() { return m_bodies; }

        /** Reads CelestialBody/Star objects from the scene into m_bodies. */
        void syncFromScene(const Core::Scene::SceneManager& scene);

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

        /** @brief NIVEL de marea (metros, con signo) en el observador: el bulto de marea sigue a
         *  la Luna (y al Sol, ~0.46×). Pleamar cuando la Luna está en el cénit o el nadir,
         *  bajamar en el horizonte. El render lo suma radialmente a la lámina del océano para que
         *  el mar "respire" (sube/baja). Amplitud suave (~±0.7 m). 0 si no hay planeta/Luna. */
        float getTideHeight(const glm::dvec3& observer) const;

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
        /** @brief Planeta activo (centro/radio en METROS, mismas unidades que las posiciones
         *  de física). Radio = nivel del mar (esfera de referencia). Para buoyancy/oclusión. */
        bool       hasActivePlanet()        const { return m_hasActivePlanet; }
        glm::dvec3 getActivePlanetCenter()  const { return m_planetCenter; }
        double     getActivePlanetRadius()  const { return m_planetRadius; }
        /** @brief Avanza la mecánica celeste un frame: órbita del Sol (día/noche),
         *  órbita de la Luna, marea y tiempo atmosférico. Lo llama el render. */
        void advanceCelestial(double dt);
        /** @brief El RELOJ del cielo: `PlanetarySystem::simulationTime` del sistema real (el mismo en
         *  todos los clientes). Lo pasa Application antes de `advanceCelestial`. */
        void setSimulationTime(double t) { m_simTime = t; m_hasSimTime = true; }
        /** @brief Sol y Luna RELATIVOS al centro del planeta activo (m), tras `advanceCelestial`: para
         *  la marea (`PlanetarySystem::setSkyTidalBodies`). Cero si no hay cielo todavia. */
        glm::dvec3 sunFromPlanet()  const { return m_sunFromPlanet; }
        glm::dvec3 moonFromPlanet() const { return m_moonFromPlanet; }

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
        std::unique_ptr<Planet::PlanetarySystem> m_planetarySystem;
        
        // El "Floating Origin" (Punto 0,0,0 real en el espacio)
        glm::dvec3 m_worldOrigin;
        
        // Lista de entidades macroscópicas (estrellas, estaciones, planetas)
        std::vector<CelestialBody> m_bodies;

        // --- Mecánica celeste: EL SOL QUIETO Y EL PLANETA QUE SE MUEVE (Planet/SkyEphemeris.h) ---
        // El planeta orbita el Sol y gira sobre su eje inclinado; la Luna orbita el planeta. Se calcula
        // en el marco inercial y se pasa al del planeta (el del juego, que no gira): el terreno, la
        // fisica y la red no se mueven. m_scene permite escribir los objetos de escena Sol y Luna.
        Core::Scene::SceneManager* m_scene = nullptr;
        bool          m_celestialInit = false;
        double        m_sunDist = 0.0;      // distancia Sol-planeta de la escena (m)
        double        m_simTime = 0.0;      // reloj compartido (setSimulationTime)
        bool          m_hasSimTime = false;
        double        m_localTime = 0.0;    // sin reloj compartido (editor): se acumula el dt
        Planet::SkyClock m_sky;             // dia/año/mes del juego y fases iniciales
        Planet::SkyState m_skyNow;          // el cielo de este frame
        glm::dvec3    m_sunFromPlanet{0.0};
        glm::dvec3    m_moonFromPlanet{0.0};

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
} // namespace Haruka::World

