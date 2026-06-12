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
    enum class TerrainMode : uint8_t {
        Procedural = 0,  // fBm noise auto-generation
        Manual     = 1,  // flat sphere, editable via future editor
    };

    struct ChunkData {
        std::vector<glm::vec3> vertices;  // relative to chunkCenter (float-safe)
        std::vector<glm::vec3> morphTargets; // decimated (parent-LOD) position per vertex, for CDLOD geomorphing
        std::vector<glm::vec3> normals;
        std::vector<glm::vec2> uvs;       // per-face UV [0,1]^2
        std::vector<glm::vec3> colors;
        std::vector<unsigned int> indices;

        // Malla del cascarón de agua (a nivel del mar, sin desplazamiento de olas
        // — el oleaje Gerstner se aplica en el vertex shader). Solo se rellena
        // cuando hasOcean. Posiciones relativas al MISMO chunkCenter que el terreno.
        std::vector<glm::vec3> waterVertices;
        std::vector<glm::vec3> waterNormals;   // radial outward (esfera lisa)
        std::vector<glm::vec2> waterParams;    // por vértice: x = nivel (km, 0=océano), y = profundidad del agua (m, para orilla/color)
        std::vector<unsigned int> waterIndices;

        PlanetChunkKey key;
        std::string planetName;
        glm::dvec3  chunkCenter{0.0};     // absolute world-space center (double)
        double      planetRadius = 1.0;   // metres, for the renderer's morph-band calc
        TerrainMode terrainMode = TerrainMode::Procedural;
        float       minElevation = 0.0f;  // km, lowest terrain elevation in this chunk
        bool        hasOcean = false;     // true if any vertex is below sea level (elev < 0)

        size_t getSizeBytes() const {
            return (vertices.size() + morphTargets.size() + normals.size() + colors.size()) * sizeof(glm::vec3) +
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