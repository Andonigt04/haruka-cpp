#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <cstdint>

namespace Haruka {

    /**
     * @brief Las 6 caras de la esfera proyectada.
     */
    enum class PlanetFace : uint8_t {
        FRONT = 0, BACK = 1, TOP = 2, BOTTOM = 3, RIGHT = 4, LEFT = 5
    };

    /**
     * @brief Identificador único de un chunk en el QuadTree.
     */
    struct PlanetChunkKey {
        PlanetFace face;
        uint8_t lod;
        uint32_t x;
        uint32_t y;

        // Necesario para usarlo como clave en std::unordered_map
        bool operator==(const PlanetChunkKey& other) const {
            return face == other.face && lod == other.lod && x == other.x && y == other.y;
        }
    };

    /**
     * @brief Contiene los datos físicos de la malla generada.
     */
    struct ChunkData {
        std::vector<glm::vec3> vertices;
        std::vector<glm::vec3> normals;
        std::vector<glm::vec3> colors;
        std::vector<unsigned int> indices;
        
        PlanetChunkKey key;
        std::string planetName;

        size_t getSizeBytes() const {
            return (vertices.size() + normals.size() + colors.size()) * sizeof(glm::vec3) + 
                   indices.size() * sizeof(unsigned int);
        }
    };

    /**
     * @brief Configuración de capas de ruido.
     */
    struct LayerSettings {
        float freq = 1.0f;
        float strength = 1.0f;
        int octaves = 5;
        float persistence = 0.5f;
        float lacunarity = 2.0f;
    };

} // namespace Haruka