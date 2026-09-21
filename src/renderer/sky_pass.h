#pragma once
/**
 * @file renderer/sky_pass.h
 * @brief EL PASE DE CIELO como modulo: el fondo (gradiente + sol + estrellas, triangulo fullscreen
 *        sin profundidad) y, con el, EL CLIMA DEL FRAME visto desde la camara: lluvia/nieve que
 *        caen, viento, mojado del suelo y nieve acumulada (integrados en el tiempo), y el aire de la
 *        perspectiva aerea. Vivia en `Application::passSky` mas siete miembros (lluvia, viento, aire).
 *
 * Los demas pases LEEN `weather()`: la lluvia (gotas y mascara cenital), el fluido (caudal), las
 * nubes y los props (viento, aire). Lo escribe SOLO este pase, una vez por frame.
 */
#include <glm/glm.hpp>

#include "rhi/rhi_types.h"
#include "rhi/rhi_resources.h"

namespace Haruka { namespace Core { class Camera; } class PlanetarySystem; class WorldSystem; }

namespace Haruka::Renderer {

class SkyPass {
public:
    struct Frame {
        Core::Camera*         camera  = nullptr;
        PlanetarySystem*      planets = nullptr;
        WorldSystem*          world   = nullptr;
        RHI::RenderPassHandle sceneTargetPass{};   ///< invalido = backbuffer
        int   renderW = 1, renderH = 1;            ///< del target de escena (con escala de render)
        uint32_t width = 1, height = 1;            ///< de la ventana/viewport (viewport del backbuffer)
        float rainOverride = -1.0f;                ///< consola `rain`: fuerza el temporal; <0 = auto
    };
    /// El clima del frame, desde la camara. Lo escribe `draw`; lo leen los demas pases.
    struct Weather {
        float     rainAmount = 0.0f;    ///< 0..1 solo LLUVIA (lo que dibuja el pase de gotas)
        float     snowAmount = 0.0f;    ///< 0..1 nieve (copos)
        glm::vec3 windVec{0.0f};        ///< el UNICO viento del mundo (marco local del planeta)
        float     windSlant = 0.0f;     ///< inclinacion de la lluvia por el viento (este)
        float     groundWetness = 0.0f; ///< 0..1 INTEGRADO: mojarse ~30 s de chaparron, secarse minutos
        float     snowAccum = 0.0f;     ///< 0..1 INTEGRADO: cuaja nevando, funde con la temperatura
        /// Perspectiva aerea (lib/aerial.glsl): x = 1/L · y = dia · z = altitud del ojo · w = escala
        /// de altura del aire. Un solo aire para terreno, props y nubes.
        glm::vec4 aerial{1.0f / 60000.0f, 1.0f, 0.0f, 0.0f};
    };

    ~SkyPass();
    void shutdown();
    /// El fondo del frame, tras el clear y antes de la escena. Escribe `weather()`.
    void draw(const Frame& f);
    const Weather& weather() const { return m_wx; }

private:
    RHI::PipelineHandle m_skyPSO{};
    RHI::BufferHandle   m_skyUBO{};   // SkyParams (binding 5)
    Weather m_wx;
};

} // namespace Haruka::Renderer
