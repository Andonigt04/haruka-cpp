#pragma once

#include <memory>
#include <glm/glm.hpp>

#include "core/scene/scene_manager.h" // Para leer terrainSettings
#include "tools/planetary_types.h" // Para PlanetChunkKey y ChunkData
#include "core/terrain/terrain_sample.h" // TerrainSample (muestreo canónico)

namespace Haruka {

    class DeformationField;

    class GpuHeightfield; // generador GPU (compute), usado SOLO en el hilo principal

    class TerrainGenerator {
    public:
        TerrainGenerator() = default;
        ~TerrainGenerator(); // out-of-line por unique_ptr<GpuHeightfield> incompleto

        // Scales the noise octave counts used per vertex (lower = cheaper chunk
        // generation, slightly smoother terrain). Driven by terrainQuality.
        // 1.0 = full detail. Applies to chunks generated after the change.
        static inline float s_detailScale = 1.0f;

        /** @brief Player-edit layer (craters/dig/build). Non-owning; may be null.
         *  Read from worker threads during generateChunk → must outlive generation
         *  and only be mutated on the main thread between frames. */
        void setDeformationField(const DeformationField* f) { m_deform = f; }

        /**
         * @brief Genera la malla de un chunk específico.
         * Esta función debería ser agnóstica al hilo (Thread-safe).
         */
        std::shared_ptr<ChunkData> generateChunk(
            const PlanetChunkKey& key,
            const nlohmann::json& settings,
            double planetRadius,
            bool mainThread = false,                       // (reservado)
            const std::vector<float>*     preElev   = nullptr, // GPU: elev km del grid
            const std::vector<glm::vec3>* preNormal = nullptr, // GPU: normales del grid
            const std::vector<float>*     preWater  = nullptr  // GPU: nivel de agua km (0=mar,>0=lago)
        );

        // ---- Orquestación GPU asíncrona (hilo principal) ----
        /** @brief ¿Hay slot GPU libre para despachar un chunk? */
        bool gpuHasFreeSlot();
        /** @brief Despacha el cómputo GPU (elev+normal) de un chunk (NO bloquea).
         *  Devuelve el id de slot, o -1 si no procede (no terran/sin GL/sin slot). */
        int  gpuDispatch(const PlanetChunkKey& key, const nlohmann::json& settings, double planetRadius);
        /** @brief Si el slot GPU terminó, lee elev+normal (GL, hilo principal) y libera
         *  el slot → true. Si aún computa, false. El ENSAMBLADO de la malla (CPU) lo
         *  hace luego generateChunk(...,preElev,preNormal) en un worker. */
        bool gpuHarvestData(int slot, std::vector<float>& outElev, std::vector<glm::vec4>& outNormal4,
                            std::vector<float>& outWater);
        /** @brief Vista de solo lectura sobre los buffers mapeados de un slot GPU terminado. */
        struct GpuMappedView { const float* elev = nullptr; const glm::vec4* norm4 = nullptr;
                               const float* water = nullptr; std::size_t count = 0; };
        /** @brief Como gpuHarvestData pero SIN copiar: devuelve los punteros mapeados para que la
         *  copia (lenta: memoria no cacheada) la haga el worker. Ver GpuHeightfield::tryMapHarvest.
         *  Si devuelve true, hay que llamar a gpuReleaseSlot(slot) al terminar. */
        bool gpuMapHarvest(int slot, GpuMappedView& out);
        /** @brief Devuelve el slot GPU al pool (thread-safe, sin GL). */
        void gpuReleaseSlot(int slot);

        /**
         * @brief Returns the terrain height offset (in metres) above the
         *        reference sphere surface at the given unit-sphere direction.
         * @param sphereDir  Normalised direction on the unit sphere.
         * @param settings   Planet terrain settings JSON (same format used by generateChunk).
         * @param planetRadius Planet radius in metres.
         */
        float sampleHeightAt(const glm::vec3& sphereDir,
                             const nlohmann::json& settings,
                             double planetRadius);

        /**
         * @brief Muestreo CANÓNICO de superficie (F3): devuelve el TerrainSample
         *        completo (elevación, agua, wetness, clima…) con las MISMAS ediciones
         *        (deform) que aplica el render. Es la "única verdad" en CPU: colisión,
         *        puntería, agua y siembra de recursos deben pasar por aquí (o por los
         *        params que expone el motor) en vez de re-muestrear por su cuenta.
         *        sampleHeightAt() es ahora un wrapper de este (devuelve elevKm·1000).
         */
        TerrainSample sampleSurfaceAt(const glm::vec3& sphereDir,
                                      const nlohmann::json& settings,
                                      double planetRadius);

        // Convierte coordenadas de Chunk (X,Y) a posición 3D en la cara del cubo (pura: sin estado).
        // PÚBLICA porque el compute la REPLICA en GLSL (u_deriveDirs) y el test las compara.
        static glm::dvec3 getLocalPosition(const PlanetChunkKey& key, int x, int y, int chunkSize);

        /**
         * @brief INVERSA de getLocalPosition: dirección esférica → (cara, lx, ly) del cubo.
         *
         * Hace falta para preguntarle la altura A LA MALLA (F10): dada una dirección, hay que saber
         * en qué CELDA de qué chunk cae. La proyección cubo→esfera (Cobb) **no se invierte en forma
         * cerrada**, así que se parte de la proyección gnómica (dividir por la componente dominante)
         * y se refina con Newton — converge en 2-3 pasos porque el mapeo es suave y casi identidad
         * en el sentido tangencial.
         *
         * @param dir  dirección unitaria (planet-local).
         * @param lx,ly coordenadas de la cara en [-1,1] (las que consume getLocalPosition).
         */
        static void dirToFaceLocal(const glm::dvec3& dir, PlanetFace& outFace,
                                   double& lx, double& ly);

    private:
        // Métodos internos para calcular ruido (Noise)
        float calculateHeight(const glm::vec3& posOnSphere, const nlohmann::json& layers);

        const DeformationField* m_deform = nullptr;
        std::unique_ptr<GpuHeightfield> m_gpu; // lazy; solo hilo principal (gpuTerrain)
    };

}