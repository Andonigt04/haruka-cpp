/**
 * @file gpu_heightfield.h
 * @brief Generador de heightfield en GPU (compute) — migración CPU→GPU.
 *
 * Despacha `shaders/terrain_gen.comp` (Perlin portado EXACTO del C++, paridad ✓)
 * para calcular, por dirección esférica, elevación (km) + normal. SOLO desde el
 * hilo con contexto GL (el principal).
 *
 * Dos modos:
 *  - generate(): SÍNCRONO (dispatch + readback bloqueante). Para el test de paridad.
 *  - dispatchAsync()/tryHarvest(): ASÍNCRONO con FENCES y un ANILLO de slots → varios
 *    chunks en vuelo a la vez sin bloquear el frame (el camino del streaming).
 */
#pragma once

#include <vector>
#include <memory>
#include <cstddef>
#include <glad/glad.h>
#include <glm/glm.hpp>

namespace Haruka { namespace Renderer { class ComputeShader; } }

namespace Haruka {

class GpuHeightfield {
public:
    GpuHeightfield();
    ~GpuHeightfield();

    // Parámetros del compute (de deriveWorldParams).
    struct Params {
        int   seed = 0;
        float continentFreqA = 1.2f, reliefStrength = 1.0f, seaThreshold = 0.0f;
        float voronoiDensity = 1.0f, lakeDensity = 150.0f, lakeMaxProb = 0.5f;
        double radius = 1.0;
    };

    /** @brief SÍNCRONO (bloquea hasta tener el resultado). Para el test de paridad. */
    bool generate(const std::vector<glm::vec3>& dirs, const Params& p,
                  std::vector<float>& outElevKm, std::vector<glm::vec3>& outNormal,
                  std::vector<float>& outWaterKm);

    // ---- Camino ASÍNCRONO (fences) ----
    static constexpr int kSlots = 8; // chunks GPU en vuelo a la vez

    /** @brief ¿Hay algún slot libre para despachar? */
    bool hasFreeSlot() const;
    /** @brief Despacha el cómputo (NO bloquea, pone un fence). Devuelve el id de slot
     *  o -1 si no hay libre / falló. */
    int  dispatchAsync(const std::vector<glm::vec3>& dirs, const Params& p);
    /** @brief Si el slot terminó (fence señalado), lee elev+normal+nivelAgua, libera el
     *  slot y devuelve true. Si aún computa, false (sin bloquear). */
    bool tryHarvest(int slot, std::vector<float>& outElevKm, std::vector<glm::vec3>& outNormal,
                    std::vector<float>& outWaterKm);

private:
    std::unique_ptr<Haruka::Renderer::ComputeShader> m_shader;
    bool m_failed = false;
    bool ensureShader();

    struct Buffers { GLuint dirs = 0, elev = 0, norm = 0, water = 0; std::size_t capacity = 0; };
    void ensureBuffers(Buffers& b, std::size_t n);
    void uploadAndDispatch(Buffers& b, const std::vector<glm::vec3>& dirs, const Params& p);
    static void readback(Buffers& b, std::size_t n, std::vector<float>& elev,
                         std::vector<glm::vec3>& normal, std::vector<float>& water);

    // Síncrono: un set de buffers propio.
    Buffers m_sync;

    // Asíncrono: anillo de slots con fence.
    struct Slot { Buffers buf; GLsync fence = nullptr; std::size_t count = 0; bool inUse = false; };
    Slot m_slot[kSlots];
};

} // namespace Haruka
