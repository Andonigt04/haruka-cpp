/**
 * @file precipitation_renderer.h
 * @brief La lluvia y la nieve, DIBUJADAS EN EL MUNDO (con profundidad), no sobre la imagen.
 *
 * ── QUÉ CAMBIA RESPECTO AL PASE ANTERIOR ────────────────────────────────────────────────────────
 * El pase de lluvia era un triángulo a pantalla completa mezclado sobre la escena YA compuesta. No
 * tenía profundidad ni posición, así que caía igual dentro de una cueva, tras un muro o bajo un
 * techo: no era lluvia, era un filtro de imagen. Este pase dibuja gotas y copos como GEOMETRÍA
 * dentro del pase de escena, con el depth buffer del mundo → lo que está delante las tapa.
 *
 * Lo que este pase NO resuelve por sí solo: estar BAJO un tejado con la lluvia entre el tejado y tu
 * cara. Ahí no hay nada delante de la gota, así que el depth no la descarta. Eso lo resuelve la
 * máscara de exposición al cielo (fase C), que este renderer ya acepta como `skyVisibility`.
 *
 * ── COSTE ───────────────────────────────────────────────────────────────────────────────────────
 * Cero buffers y cero trabajo por frame en CPU: la posición de cada gota sale de un hash de
 * `gl_VertexID` en el vertex shader (ver precip.vert). Un draw, `6·N` vértices, N ≈ 3-6 mil.
 */
#pragma once

#include <glm/glm.hpp>
#include "rhi/rhi_types.h"

namespace Haruka {

class PrecipitationRenderer {
public:
    struct Params {
        glm::dvec3 cameraPos{0.0};     ///< posición absoluta (para anclar la rejilla al MUNDO)
        glm::vec3  up{0, 1, 0};        ///< cénit local (radial en un planeta)
        glm::vec3  lookDir{0, 0, -1};  ///< dirección de vista
        glm::vec3  wind{0.0f};         ///< viento del clima (m/s)
        glm::vec3  sunColor{1.0f};     ///< color de la luz (la gota lo recoge; de noche no es blanca)
        float      amount = 0.0f;      ///< [0,1] intensidad. 0 = no se dibuja nada.
        bool       snow = false;       ///< nieve (copo lento) en vez de lluvia (estela rápida)
        float      skyVisibility = 1.0f; ///< [0,1] cuánto cielo ve el observador (fase C). 0 = bajo techo.
        double     timeSeconds = 0.0;  ///< reloj del mundo
        int        viewportHeightPx = 1080; ///< alto del target en píxeles: fija el ancho MÍNIMO de la
                                            ///< gota (una gota real es sub-píxel y no se rasterizaría)
        /** @brief MÁSCARA CENITAL (fase C): profundidad de lo que hay ENCIMA, y su matriz. Con ella
         *  cada gota comprueba si está bajo cubierto y se descarta — por GOTA, no por un interruptor
         *  global: en el borde de un tejado llueve fuera y no dentro, en la misma imagen. Handle
         *  vacío = sin máscara → llueve en todas partes (comportamiento de la fase B). */
        RHI::TextureHandle skyMask{};
        glm::mat4          skySpace{1.0f};
    };

    /** @brief Dibuja la precipitación. No hace nada si `amount·skyVisibility` es despreciable.
     *  Debe llamarse DENTRO del pase de escena (con su depth), después de lo opaco. */
    void render(const Params& p);

private:
    RHI::PipelineHandle m_pso;
    RHI::BufferHandle   m_ubo;
    bool                m_failed = false;   // el shader no linkó: no reintentar cada frame
};

} // namespace Haruka
