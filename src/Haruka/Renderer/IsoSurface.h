#pragma once
/**
 * @file IsoSurface.h
 * @brief De un campo escalar en la GPU a una malla dibujable (`assets/shaders/iso_surface.comp`,
 *        surface nets). Genérico: el líquido de `ApicFluid` es el primer usuario, pero vale para
 *        cualquier campo en nodos (densidad de nube, cueva...).
 *
 * Salida: `vertices()` (por vértice: vec4 posición, vec4 normal, en el marco que diga `origin`),
 * `indices()` (identidad 0..capacidad) y `command()` (DrawElementsIndirect, cuenta escrita por la
 * GPU). Se dibuja con `drawIndexedIndirect(command(), 1, 20)` sin leer nada en la CPU.
 */
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

#include "Haruka/RHI/RHIResources.h"

namespace Haruka { namespace RHI { class Context; class Device; } }

namespace Haruka::Renderer {

class IsoSurface {
public:
    struct Grid {
        glm::ivec3 nodes{ 0 };       ///< nodos del campo (x más rápido)
        bool  periodicZ = false;
        float cell = 1.0f;           ///< lado de celda (m)
        float iso = 0.5f;            ///< dentro = campo > iso
        glm::vec3 origin{ 0.0f };    ///< posición del nodo (0,0,0) en el marco de salida
        /// Bordes x/z ABIERTOS: fuera de la rejilla el campo sigue como en su borde (no hay pared de
        /// superficie). Para un trozo de algo que continúa fuera (el agua de un parche). Cerrado = 0 fuera.
        bool openSides = false;
    };
    struct Vertex { glm::vec4 p, n; };

    IsoSurface() = default;
    ~IsoSurface() { shutdown(); }
    IsoSurface(const IsoSurface&) = delete;
    IsoSurface& operator=(const IsoSurface&) = delete;

    /// `capacity`: vértices máximos (múltiplo de 3). Lo que no cabe se descarta (la cuenta se recorta).
    bool init(RHI::Device* dev, uint32_t capacity);
    void shutdown();

    /// Graba la extracción (fuera de un render pass). Rehace los buffers de celda si cambia la rejilla.
    void extract(RHI::Context* ctx, RHI::BufferHandle field, const Grid& g);

    RHI::BufferHandle vertices() const { return m_out; }
    RHI::BufferHandle indices() const  { return m_idx; }
    RHI::BufferHandle command() const  { return m_cmd; }
    static constexpr uint32_t kCommandStride = 20;

    /// UBO del dibujo (binding 0 de `iso_mesh.vert`): marco local relativo al ojo + libre del material.
    struct DrawUBO {
        glm::mat4 viewProj{ 1.0f };          ///< cámara-relativa
        glm::vec4 originEye{ 0.0f };         ///< origen del marco local relativo al ojo (m)
        glm::vec4 axisX{ 1, 0, 0, 0 }, axisY{ 0, 1, 0, 0 }, axisZ{ 0, 0, 1, 0 };
        glm::vec4 misc{ 0.0f };              ///< del material (water_surface: x = profundidad)
    };
    /// Dibuja lo último extraído con `pipe` (vertex `iso_mesh.vert` + el fragment del material),
    /// dentro de un render pass. `drawUBO` es del que llama.
    void draw(RHI::Context* ctx, RHI::PipelineHandle pipe, RHI::BufferHandle drawUBO) const;

    /// Diagnóstico sin esperar: copia la cuenta de ESTE frame a un buffer de lectura y devuelve la de
    /// la última copia que ya llegó (unos frames de retraso). −1 si aún no hay ninguna.
    int64_t pollCount(RHI::Context* ctx);

    /// Para tests: los vértices escritos (copia + espera, dos frames).
    bool readBack(std::vector<Vertex>& out, uint32_t& written);

private:
    void ensureCells(size_t cells);

    RHI::Device* m_dev = nullptr;
    uint32_t m_capacity = 0;
    size_t   m_cells = 0;
    RHI::PipelineHandle m_pipe{};
    RHI::BufferHandle m_params{}, m_pass[4]{}, m_cell{}, m_out{}, m_idx{}, m_cmd{}, m_cmdRb{};
    bool m_polled = false;
};

} // namespace Haruka::Renderer
