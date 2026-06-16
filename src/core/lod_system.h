#pragma once

#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <memory>
#include <functional>
#include "scene/scene_manager.h"
#include "tools/planetary_types.h"

namespace Haruka {

    // Estructura de salida para el StreamingSystem
    struct LODUpdate {
        std::string planetName;
        std::vector<PlanetChunkKey> chunksToLoad;
        std::vector<PlanetChunkKey> chunksToKeep;
        std::vector<PlanetChunkKey> chunksToUnload;
        // true si la carga PROGRESIVA bloqueó alguna subdivisión porque el chunk grueso
        // aún no está residente (quiero afinar pero el grueso carga). El llamador debe
        // seguir recalculando el LOD (saltarse el throttle) hasta que deje de estarlo →
        // si no, con la cámara quieta el planeta se queda en grueso para siempre.
        bool residencyLimited = false;
    };

    class LODSystem {
    public:
        LODSystem(double splitFactor = 1.5, int maxLOD = 12);

        /** @brief Tunes LOD aggressiveness (lower = fewer chunks = cheaper). Takes
         *  effect on the next recompute. */
        void   setParams(double splitFactor, int maxLOD) { m_splitFactor = splitFactor; m_maxLOD = maxLOD; }
        double getSplitFactor() const { return m_splitFactor; }
        int    getMaxLOD()      const { return m_maxLOD; }

        /** @brief LOD MÍNIMO del planeta entero (suelo): todas las caras se subdividen
         *  hasta este nivel SIEMPRE, sin importar la distancia ni la altitud → el planeta
         *  completo está siempre cargado a este detalle base (≈ 6·4^minLOD chunks), y la
         *  cámara refina por encima. minLOD=4 ≈ 1536 piezas; 3 ≈ 384. */
        void setMinLOD(int minLOD) { m_minLOD = minLOD; }
        int  getMinLOD() const { return m_minLOD; }

        /**
         * @brief Analiza un objeto planetario y genera las órdenes de streaming.
         */
        // 'isResident' (opcional): predicado que dice si un chunk ya está en GPU. Si se
        // pasa, el LOD CARGA PROGRESIVAMENTE: solo subdivide un nodo cuando él mismo (o
        // sus hijos) ya está residente → siempre hay un chunk grueso dibujado mientras
        // los finos generan (sin huecos negros) y la carga sube grueso→fino suavemente.
        using ResidencyFn = std::function<bool(const PlanetChunkKey&)>;
        // 'bodyId' identifica el cuerpo celeste → se estampa en TODAS las claves que el
        // árbol produce, para que cuerpos distintos no colisionen en caché/renderer.
        LODUpdate updatePlanetLOD(const std::shared_ptr<SceneObject>& planet, const glm::dvec3& cameraPos,
                                  const ResidencyFn& isResident = nullptr, uint16_t bodyId = 0);

        /**
         * @brief Forgets a chunk from the "last frame" set so the next update
         * re-classifies it as chunksToLoad (not chunksToKeep). Used after a terrain
         * edit invalidates a chunk: keep-only would call cache.getChunk (now empty)
         * and never re-upload to GPU → the chunk would vanish. Forgetting it forces
         * a reload.
         */
        void forgetChunk(const PlanetChunkKey& key);

        /** @brief True if the key is in the current visible set. Used to drop chunks
         *  whose async generation finished AFTER they left view (otherwise they pile
         *  up on the GPU forever — overlapping terrain + growing cost). */
        bool isVisible(const PlanetChunkKey& key) const;

        /** @brief Mapea (cara, u,v ∈ [0,1], radio) a una posición en la esfera
         *  (planet-local). Matemática pura — útil para ordenar chunks por distancia. */
        static glm::dvec3 getCubeToSpherePos(PlanetFace face, double u, double v, double radius);

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
        int m_minLOD = 4; // suelo: el planeta entero siempre a ≥ este LOD (≈1536 chunks)
        uint16_t m_currentBody = 0; // cuerpo de la pasada actual (estampa todas las claves)

        // hash → key, para poder reconstruir la key al hacer unload
        std::unordered_map<uint64_t, PlanetChunkKey> m_lastFrameChunks;
        std::unordered_map<uint64_t, PlanetChunkKey> m_currentFrameChunks;

        void recursiveProcess(LODNode* node, const glm::dvec3& cameraPos, LODUpdate& update, double radius, const glm::dvec3& planetPos, const ResidencyFn& isResident);
        void subdivide(LODNode* node, double radius, const glm::dvec3& planetPos);

        // 2:1 LOD balance sobre el conjunto de hojas (m_currentFrameChunks).
        void balanceLeaves();
        bool findCoveringLeaf(PlanetFace face, int lod, uint32_t x, uint32_t y, PlanetChunkKey& out) const;
        void subdivideLeafKey(const PlanetChunkKey& k);

    };

} // namespace Haruka