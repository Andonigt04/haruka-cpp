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
#include <atomic>
#include <cstddef>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include "rhi/rhi_types.h"

namespace Haruka {

    /** @brief SSBO del CAMPO EROSIONADO del planeta (elev/caudal/lago por celda del cube-sphere).
     *  Lo genera C++ una vez por seed y lo comparten el compute de terreno y el shader de AGUA:
     *  el agua debe saber DÓNDE HAY AGUA (si no, la lámina del océano existe también bajo la tierra
     *  y la ves al cavar). Devuelve el id GL y escribe la resolución de cara en `outRes`. */
    unsigned int planetFieldsSSBO(uint32_t seed, int& outRes);

    /** @brief SSBO del CLIMA del planeta: por celda, `vec4(tempC, humedad, 0, 0)`.
     *  Va aparte del campo (que ya usa sus 4 canales) y lo consume `planet.frag` para el BIOMA:
     *  el bioma sale de temperatura + humedad + pendiente + caudal (Whittaker), no de la altura.
     *  Misma rejilla y misma resolución que el campo → se muestrea con el mismo código. */
    unsigned int planetClimateSSBO(uint32_t seed, int& outRes);

    /** @brief ¿Está activado el nivel MESO? (HARUKA_MESO=1). Apagado = el mundo usa solo el macro. */
    bool mesoEnabled();
    /** @brief El campo macro del planeta (una sola instancia por seed; la comparten todos). */
    class PlanetFields;
    PlanetFields& planetFieldsFor(uint32_t seed);
 namespace Renderer { class ComputeShader; } }

namespace Haruka {

class GpuHeightfield {
public:
    GpuHeightfield();
    ~GpuHeightfield();

    // Parámetros del compute (de deriveWorldParams).
    struct Params {
        int   seed = 0;
        float continentFreqA = 1.2f, reliefStrength = 1.0f, seaThreshold = 0.0f;
        float voronoiDensity = 1.0f, lakeDensity = 120.0f, lakeMaxProb = 0.15f; // lakeMaxProb 0.5→0.15 (menos lagos)
        float coastWidth = 1.0f; // factor de ancho de costa (seed): escala rampas distToCoast
        float vertexSpacingM = 2.0f; // separación entre vértices del chunk (m) → eps de la normal (LOD-aware, mata pinchos)
        double radius = 1.0;

        // Descripción del chunk para que la GPU DERIVE las direcciones (deriveDirs=true) en vez de
        // recibir la rejilla: construirla en CPU y subirla era el pico de `scan.dispatch`.
        // El camino síncrono (paridad) pasa direcciones sueltas → deriveDirs=false.
        bool deriveDirs = false;
        int  gridRes = 0, face = 0, lod = 0, chunkX = 0, chunkY = 0;
    };

    /** @brief SÍNCRONO (bloquea hasta tener el resultado). Para el test de paridad. */
    bool generate(const std::vector<glm::vec3>& dirs, const Params& p,
                  std::vector<float>& outElevKm, std::vector<glm::vec3>& outNormal,
                  std::vector<float>& outWaterKm);

    // ---- Camino ASÍNCRONO (fences) ----
    static constexpr int kSlots = 16; // chunks GPU en vuelo a la vez (subido de 8: genera ~2× más
                                      // rápido → el primer vistazo a un ángulo nuevo tarda la mitad;
                                      // el compute es barato/GPU ociosa, los buffers extra son pequeños)

    /** @brief ¿Hay algún slot libre para despachar? */
    bool hasFreeSlot() const;
    /** @brief Despacha el cómputo (NO bloquea, pone un fence). Devuelve el id de slot
     *  o -1 si no hay libre / falló. Con p.deriveDirs, `dirs` puede ir VACÍO: solo se usa
     *  `count` (nº de vértices) y la GPU calcula las direcciones. */
    int  dispatchAsync(const std::vector<glm::vec3>& dirs, const Params& p, std::size_t count = 0);
    /** @brief Si el slot terminó (fence señalado), lee elev+normal+nivelAgua, libera el
     *  slot y devuelve true. Si aún computa, false (sin bloquear).
     *
     *  Las normales salen CRUDAS (vec4, como las escribe el compute): convertirlas a vec3 es un
     *  bucle por vértice y esto corre en el hilo de RENDER. Que lo haga el worker que ensambla
     *  la malla — aquí solo se hacen 3 memcpy desde los buffers mapeados. */
    bool tryHarvest(int slot, std::vector<float>& outElevKm, std::vector<glm::vec4>& outNormal4,
                    std::vector<float>& outWaterKm);

    /** @brief Punteros MAPEADOS de un slot terminado, para copiarlos DESDE OTRO HILO.
     *
     *  Los buffers de readback viven en memoria no cacheada: leerlos va a ~cientos de MB/s, así que
     *  copiar 16 chunks en el hilo de render costaba varios ms. Con esto el hilo de render solo
     *  comprueba el fence (barato) y el WORKER hace la copia lenta. El slot sigue ocupado hasta que
     *  el worker llama a releaseSlot() → nadie puede sobreescribir los datos que está leyendo.
     *
     *  Devuelve false si el slot aún computa. Si devuelve true, el llamador SE COMPROMETE a llamar
     *  a releaseSlot(slot) cuando termine con los punteros. Solo hilo de render (toca GL). */
    struct MappedView { const float* elev = nullptr; const glm::vec4* norm4 = nullptr;
                        const float* water = nullptr; std::size_t count = 0; };
    bool tryMapHarvest(int slot, MappedView& out);
    /** @brief Devuelve el slot al pool. Thread-safe: NO toca GL (la fence ya se borró). */
    void releaseSlot(int slot);

private:
    std::unique_ptr<Haruka::Renderer::ComputeShader> m_shader;
    bool m_failed = false;
    bool ensureShader();

    struct Buffers {
        GLuint dirs = 0, elev = 0, norm = 0, water = 0;   // ids GL nativos (cache)
        Haruka::RHI::BufferHandle hDirs, hElev, hNorm, hWater;  // handles RHI (vacíos en fallback)
        std::size_t capacity = 0;
    };
    void ensureBuffers(Buffers& b, std::size_t n);
    void uploadAndDispatch(Buffers& b, const std::vector<glm::vec3>& dirs, const Params& p,
                           std::size_t count = 0);
    static void readback(Buffers& b, std::size_t n, std::vector<float>& elev,
                         std::vector<glm::vec4>& normal4, std::vector<float>& water);

    // Síncrono: un set de buffers propio.
    Buffers m_sync;

    // Asíncrono: anillo de slots con fence.
    // `inUse` es ATÓMICO porque el slot lo LIBERA el worker (releaseSlot) cuando termina de copiar
    // desde la memoria mapeada, mientras el hilo de render busca slots libres para despachar.
    struct Slot { Buffers buf; GLsync fence = nullptr; std::size_t count = 0; std::atomic<bool> inUse{false}; };
    Slot m_slot[kSlots];
};

} // namespace Haruka
