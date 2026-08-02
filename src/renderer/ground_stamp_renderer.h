/**
 * @file ground_stamp_renderer.h
 * @brief Pinta la lista de huellas de `GroundLayer` en una ventana cenital que el terreno muestrea.
 *
 * Es deliberadamente pequeño: un render target de color (sin profundidad) de 512², la ventana de
 * ±`GroundLayer::kWindowM` alrededor del jugador, y un draw de `6·kMaxStamps` vértices sin buffers.
 * Toda la inteligencia (qué huellas viven, cuánto se han desvanecido, de qué material son) está en
 * `GroundLayer`, que es GL-free y la puede compartir el servidor.
 *
 * ⚠️ La proyección a coordenadas de ventana se hace en **DOUBLE** en la CPU: las posiciones son
 * absolutas sobre un planeta de 6.4e6 m y en float32 dos pisadas consecutivas (0.75 m) caerían en el
 * mismo valor. El shader solo recibe coordenadas ya pequeñas y relativas.
 */
#pragma once

#include <glm/glm.hpp>
#include "rhi/rhi_types.h"
#include "core/ground_layer.h"

namespace Haruka {

class GroundStampRenderer {
public:
    /** @brief Lado de la textura de la ventana. 512 sobre ±24 m = 9.4 cm/téxel: una huella de 22 cm
     *  ocupa ~5 téxeles, suficiente para leerse con su reborde. */
    static constexpr unsigned kRes = 512u;

    /** @brief Re-pinta la ventana alrededor de `center`. `up` = cénit local; `now` = reloj del mundo.
     *  @return false si no hay nada que pintar (ninguna huella viva) o si el pase no está disponible. */
    bool render(const GroundLayer& layer, const glm::dvec3& center, const glm::vec3& up, double now);

    /** @brief Textura de huellas (rojo = hundido, verde = reborde). Válida tras un `render` con éxito. */
    RHI::TextureHandle texture() const { return m_tex; }
    /** @brief Matriz que lleva una posición RELATIVA A LA CÁMARA a la ventana (la usa el terreno). */
    const glm::mat4& stampSpace() const { return m_stampSpace; }

private:
    RHI::PipelineHandle   m_pso;
    RHI::BufferHandle     m_ubo;
    RHI::RenderPassHandle m_pass;
    RHI::TextureHandle    m_tex;
    glm::mat4             m_stampSpace{1.0f};
    bool                  m_failed = false;
};

} // namespace Haruka
