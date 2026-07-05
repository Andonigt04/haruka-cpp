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

        // --- LOD v3 F1: split por ERROR EN PANTALLA (conmutable; OFF = baseline distancia) ---
        // Un nodo se subdivide hasta que su tamaño proyecta ≤ targetPx en pantalla. Acotado por
        // la resolución (no por el mundo) y SIN tope por altitud (ese era el hack que volvía
        // grueso a distancia). screenK = viewportH / (2·tan(fovY/2)) (px por unidad/dist=1),
        // se fija por frame desde la cámara. Ver docs/guides/PLAN_LOD_V3.md.
        void   setScreenSpaceLOD(bool on) { m_screenSpace = on; }
        bool   getScreenSpaceLOD() const  { return m_screenSpace; }
        void   setScreenK(double k)       { if (k > 1.0) m_screenK = k; }
        void   setTargetPx(double px)     { if (px > 1.0) m_targetPx = px; }
        double getTargetPx() const        { return m_targetPx; }

        // --- BIAS DE COSTA: subdividir MÁS donde el chunk cruza la línea de mar (elev≈0) ---
        // Los triángulos gruesos en la orilla hacían la línea de agua DENTADA (facetas). Con esto,
        // un chunk cuya costa cae dentro de su huella (|distToCoast| < size·k) usa un targetPx menor
        // (×coastRefine) → se subdivide ~1-2 niveles más SOLO en la franja costera (coste acotado).
        // coastFn(dir) = |distancia a la costa| en metros (misma función que el terreno); nula = sin bias.
        void setCoastBias(std::function<double(const glm::dvec3&)> coastFn, double refine = 0.4) {
            m_coastFn = std::move(coastFn); m_coastRefine = (refine > 0.05 && refine < 1.0) ? refine : 0.4;
        }

        // F2: frustum (cam-rel VP) para ACOTAR el recompute. En modo screen-space, los nodos
        // fuera de la vista o tras el horizonte NO se refinan (si no, el LOD subdivide la
        // esfera ENTERA en CPU = pico de cientos de ms; el horizon cull del render no ayuda
        // aquí, eso es solo en el dibujo). Se fija por frame (1 frame de desfase: irrelevante).
        void setCullMatrix(const glm::mat4& camRelViewProj);

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
        // (El antiguo LODNode heap-alocado se eliminó: recursiveProcess ahora recursa por VALOR
        //  key/center/size, sin construir un árbol en el heap cada recompute → sin alloc/free churn.)

        double m_splitFactor;
        int m_maxLOD;
        int m_minLOD = 4; // suelo: el planeta entero siempre a ≥ este LOD (≈1536 chunks)
        bool   m_screenSpace = true;  // F1 por DEFECTO (mata el tope por altitud). `lodscreen off` vuelve a baseline.
        double m_screenK     = 935.0; // px por (unidad de mundo / distancia); se fija por frame
        double m_targetPx    = 320.0; // subdivide si el chunk proyecta > este tamaño (px)
        std::function<double(const glm::dvec3&)> m_coastFn; // |distToCoast| m (bias de costa); vacía = off
        double m_coastRefine = 0.4;   // factor de targetPx en chunks costeros (más fino)
        glm::vec4 m_cullPlanes[6];    // F2: 6 planos del frustum (cam-rel) para acotar el recompute
        bool   m_hasCull = false;
        glm::dvec3 m_curCamPos{0.0}, m_curPlanetPos{0.0}; // estado del frame para balanceLeaves
        double m_curRadius = 1.0;
        // True si la hoja es VISIBLE (frustum). El balance 2:1 NO cascadea hacia hojas
        // invisibles (fuera del frustum) → evita la explosión de chunks en el borde del
        // frustum (acantilado de LOD que el balance intentaba suavizar = pico de cientos de ms).
        bool leafVisibleForBalance(const PlanetChunkKey& k) const;
        uint16_t m_currentBody = 0; // cuerpo de la pasada actual (estampa todas las claves)

        // hash → key, para poder reconstruir la key al hacer unload
        std::unordered_map<uint64_t, PlanetChunkKey> m_lastFrameChunks;
        std::unordered_map<uint64_t, PlanetChunkKey> m_currentFrameChunks;

        void recursiveProcess(const PlanetChunkKey& key, const glm::dvec3& center, double size,
                              const glm::dvec3& cameraPos, LODUpdate& update, double radius, const glm::dvec3& planetPos, const ResidencyFn& isResident);

        // 2:1 LOD balance sobre el conjunto de hojas (m_currentFrameChunks).
        void balanceLeaves();
        bool findCoveringLeaf(PlanetFace face, int lod, uint32_t x, uint32_t y, PlanetChunkKey& out) const;
        void subdivideLeafKey(const PlanetChunkKey& k);

    };

} // namespace Haruka