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
        // Identidad del CUERPO celeste (Tierra/Luna/Júpiter…). Sin esto, dos cuerpos con
        // el mismo (face,lod,x,y) colisionaban en la caché/renderer compartidos. Default 0
        // → las construcciones antiguas siguen compilando; las claves DERIVADas (hijos/
        // padre) deben propagar `body` explícitamente. (LOD v2 — fase F1.)
        uint16_t body = 0;

        // Necesario para usarlo como clave en std::unordered_map
        bool operator==(const PlanetChunkKey& other) const {
            return body == other.body && face == other.face && lod == other.lod && x == other.x && y == other.y;
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
        std::vector<glm::vec3> morphNormals; // decimated (parent-LOD) normal por vértice → se mezcla con la posición en el morph CDLOD (si no, el chunk aplanado conserva normales bumpy → escalón de sombreado)
        // #4 EMPAQUETADO: al final de la generación las normales se EMPAQUETAN aquí
        // (INT_2_10_10_10, 4B vs 12B) y los arrays vec3 de arriba se LIBERAN → la cache y la
        // VRAM guardan lo pequeño. El render sube estos. (Las vec3 solo existen durante la gen.)
        std::vector<uint32_t> normalsPacked, morphNormalsPacked;
        std::vector<glm::vec2> uvs;       // per-face UV [0,1]^2 (solo durante la gen; luego empaquetadas)
        std::vector<uint32_t> uvsPacked;  // #4: uv en half-float ×2 (4B vs 8B), uv∈[0,1] → precisión de sobra
        std::vector<glm::vec3> colors;
        // Índices del terreno: topología FIJA por resolución → idénticos en todos los chunks del
        // mismo res. Al final de la generación se REGISTRAN en SharedIndexTable (una vez por
        // indexCount) y este vector se LIBERA → la caché RAM no duplica ~3456 índices/chunk. El
        // render los lee de SharedIndexTable vía `indexCount`. (Idea del EBO compartido, en RAM.)
        std::vector<unsigned int> indices;
        uint32_t indexCount = 0;   // nº de índices (se conserva tras liberar `indices`)

        // Malla del cascarón de agua (a nivel del mar, sin desplazamiento de olas
        // — el oleaje Gerstner se aplica en el vertex shader). Solo se rellena
        // cuando hasOcean. Posiciones relativas al MISMO chunkCenter que el terreno.
        std::vector<glm::vec3> waterVertices;
        std::vector<glm::vec3> waterMorphTargets; // CDLOD: posición del agua en el LOD padre (decimado), para geomorph sin popping
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

        // Tamaño REAL en RAM (todos los arrays). Lo usa la caché LRU para su presupuesto:
        // si subestima (omitir uvs/morph/agua), retiene MUCHOS más chunks que el cap y se
        // come toda la RAM. Incluye terreno + agua + uvs/params.
        size_t getSizeBytes() const {
            return (vertices.size() + morphTargets.size() + normals.size() + morphNormals.size() + colors.size()
                    + waterVertices.size() + waterMorphTargets.size() + waterNormals.size()) * sizeof(glm::vec3)
                 + (uvs.size() + waterParams.size()) * sizeof(glm::vec2)
                 + (normalsPacked.size() + morphNormalsPacked.size() + uvsPacked.size()) * sizeof(uint32_t) // #4 empaquetado
                 + (indices.size() + waterIndices.size()) * sizeof(unsigned int);
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