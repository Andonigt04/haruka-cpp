#pragma once

#include <vector>
#include <unordered_set>
#include <memory>
#include "scene/scene_manager.h"
#include "tools/planetary_types.h"

namespace Haruka {

    // Estructura de salida para el StreamingSystem
    struct LODUpdate {
        std::string planetName;
        std::vector<PlanetChunkKey> chunksToLoad;
        std::vector<PlanetChunkKey> chunksToKeep;
        std::vector<PlanetChunkKey> chunksToUnload;
    };

    class LODSystem {
    public:
        LODSystem(double splitFactor = 1.5, int maxLOD = 12);

        /**
         * @brief Analiza un objeto planetario y genera las órdenes de streaming.
         */
        LODUpdate updatePlanetLOD(const std::shared_ptr<SceneObject>& planet, const glm::dvec3& cameraPos);

    private:
        struct LODNode {
            PlanetChunkKey key;
            glm::dvec3 center;
            double size;
            bool isSubdivided = false;
            std::unique_ptr<LODNode> children[4];

            LODNode(PlanetChunkKey k, glm::dvec3 c, double s) : key(k), center(c), size(s) {}
        };

        double m_splitFactor;
        int m_maxLOD;

        // Estado persistente para saber qué estaba cargado en el frame anterior
        std::unordered_set<uint64_t> m_lastFrameChunks;
        std::unordered_set<uint64_t> m_currentFrameChunks;

        void recursiveProcess(LODNode* node, const glm::dvec3& cameraPos, LODUpdate& update, double radius);
        void subdivide(LODNode* node, double radius);
        
        // Helpers para calcular posiciones en la esfera
        glm::dvec3 getCubeToSpherePos(PlanetFace face, double u, double v, double radius);
    };

} // namespace Haruka