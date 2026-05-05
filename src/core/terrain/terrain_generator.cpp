#include "terrain_generator.h"

#include "noise_generator.h" // Para generar ruido procedural


namespace Haruka {

// Global (thread-local) seed used by calculateHeight when the JSON node
// passed doesn't provide direct access to the parent config object.
namespace {
    thread_local int g_current_seed = 42;
}

    glm::vec3 TerrainGenerator::getLocalPosition(const PlanetChunkKey& key, int x, int y, int chunkSize) {
        // 1. Calcular coordenadas normalizadas [-1, 1] dentro de la cara del cubo
        double invSize = 1.0 / double(chunkSize);
        double localX = (double(x) * invSize * 2.0) - 1.0;
        double localY = (double(y) * invSize * 2.0) - 1.0;

        // 2. Proyectar según la cara (Face: 0-5)
        glm::dvec3 p;
        switch (key.face) {
            case 0: p = { 1.0,    localY, -localX }; break; // Front
            case 1: p = { -1.0,   localY,  localX }; break; // Back
            case 2: p = { localX,  1.0,   -localY }; break; // Top
            case 3: p = { localX, -1.0,    localY }; break; // Bottom
            case 4: p = { localX,  localY,  1.0 };    break; // Right
            case 5: p = { -localX, localY, -1.0 };    break; // Left
        }

        // 3. Convertir cubo a esfera (Spherify) para evitar distorsión en las esquinas
        double x2 = p.x * p.x;
        double y2 = p.y * p.y;
        double z2 = p.z * p.z;
        
        glm::dvec3 spherePos;
        spherePos.x = p.x * sqrt(1.0 - y2 / 2.0 - z2 / 2.0 + y2 * z2 / 3.0);
        spherePos.y = p.y * sqrt(1.0 - z2 / 2.0 - x2 / 2.0 + z2 * x2 / 3.0);
        spherePos.z = p.z * sqrt(1.0 - x2 / 2.0 - y2 / 2.0 + x2 * y2 / 3.0);

        return glm::vec3(spherePos);
    }

    std::shared_ptr<PlanetarySystem::ChunkData> TerrainGenerator::generateChunk(const PlanetChunkKey& key, const nlohmann::json& settings, double planetRadius) 
    {
        auto chunk = std::make_shared<PlanetarySystem::ChunkData>();
        int res = settings["config"].value("chunkSize", 64);
        // Set global seed so calculateHeight can read it when only the
        // layers json is passed (no parent pointer available).
        g_current_seed = settings["config"].value("seed", 42);
        
        // Reservar memoria para optimizar
        chunk->vertices.reserve((res + 1) * (res + 1));

        // 1. Generar Vértices
        for (int y = 0; y <= res; ++y) {
            for (int x = 0; x <= res; ++x) {
                // Obtener posición base en la esfera unitaria
                glm::vec3 posOnSphere = getLocalPosition(key, x, y, res);
                
                // Calcular altura procedural (Noise)
                float height = calculateHeight(posOnSphere, settings["config"]["layers"]);
                
                // Posición final: (Dirección * (Radio + Elevación))
                glm::vec3 finalPos = posOnSphere * (float(planetRadius) + height);
                
                chunk->vertices.push_back(finalPos);
                // Aquí también podrías calcular normales y UVs
            }
        }

        // 2. Generar Índices (Triángulos)
        for (int y = 0; y < res; ++y) {
            for (int x = 0; x < res; ++x) {
                 int i = x + y * (res + 1);
                chunk->indices.push_back(i);
                chunk->indices.push_back(i + res + 1);
                chunk->indices.push_back(i + 1);

                chunk->indices.push_back(i + 1);
                chunk->indices.push_back(i + res + 1);
                chunk->indices.push_back(i + res + 2);
            }
        }

        return chunk;
    }

    float TerrainGenerator::calculateHeight(const glm::vec3& pos, const nlohmann::json& layers) {
        float finalHeight = 0.0f;
        float continentMask = 0.0f;
        
        // Obtener seed: si la capa proporciona su propio seed, úsalo,
        // de lo contrario usa el seed global establecido por generateChunk.
        int seed = layers.value("seed", g_current_seed);

        // 1. Capa de CONTINENTES (Define la forma base: tierra vs océano)
        if (layers.contains("continents")) {
            const auto& c = layers["continents"];
            
            // Usamos fBm para obtener formas orgánicas grandes
            continentMask = NoiseGenerator::fBm(
                pos,
                seed,
                c.value("octaves", 6),
                0.5f, // Persistence estándar
                2.0f, // Lacunarity estándar
                c.value("freq", 1.0f)
            );

            // Escalamos por la fuerza definida
            finalHeight += continentMask * c.value("strength", 0.1f);
        }

        // 2. Capa de MONTAÑAS (Detalle de alta frecuencia)
        if (layers.contains("mountains")) {
            const auto& m = layers["mountains"];
            
            // Solo generamos montañas donde el continente está por encima de un umbral (máscara)
            // Esto evita que aparezcan picos en medio del océano
            if (continentMask > -0.1f) { 
                float mountValue = NoiseGenerator::fBm(
                    pos,
                    seed + 123, // Offset de seed para que no coincida con los continentes
                    m.value("octaves", 8),
                    0.45f,
                    2.1f,
                    m.value("freq", 4.0f)
                );

                // Efecto de "picos": elevar al cuadrado el ruido hace que los valles sean planos 
                // y las crestas afiladas.
                mountValue = std::pow(std::abs(mountValue), 2.0f);
                
                finalHeight += mountValue * m.value("strength", 0.05f) * (continentMask + 0.1f);
            }
        }

        return finalHeight;
    }
}