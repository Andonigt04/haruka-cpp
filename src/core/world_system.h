#pragma once

#include "tools/math_types.h"
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include "game/planetary_system.h" // El World posee al PlanetarySystem
#include "tools/planetary_types.h" // El World posee al PlanetaryTypes
#include "tools/object_types.h" // Para ObjectType en CelestialBody

namespace Haruka {

    /**
     * @brief Representa una entidad macroscópica en el vacío del espacio.
     */
    struct CelestialBody {
        std::string name;
        ObjectType type;
        WorldPos worldPos;     // Posición en double (km)
        glm::vec3 velocity;    // km/s
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

    private:
        // Sistemas coordinados por el Mundo
        std::unique_ptr<PlanetarySystem> m_planetarySystem;
        
        // El "Floating Origin" (Punto 0,0,0 real en el espacio)
        glm::dvec3 m_worldOrigin;
        
        // Lista de entidades macroscópicas (estrellas, estaciones, planetas)
        std::vector<CelestialBody> m_bodies;
    };

}