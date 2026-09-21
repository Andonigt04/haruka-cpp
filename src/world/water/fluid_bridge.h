#pragma once
/**
 * @file world/water/fluid_bridge.h
 * @brief EL ENGANCHE DEL AGUA DINAMICA (FluidHost: rios, lagos, charcos, splash) AL PLANETA: el
 *        lecho (con el fondo de las bocas de cueva), el campo de lagos horneado, la rugosidad del
 *        bioma, la marea, la publicacion del agua interior al mar, el caudal de la lluvia, y el
 *        ciclo (evapora → vapor del clima, infiltra). Vivia en `Application::passFluid` como 170
 *        lineas de lambdas que capturaban `this`.
 *
 * Puerta rapida del cuerpo seco: sin texeles bajo el nivel del mar no se construye el host siquiera.
 */
#include <memory>

#include "rhi/rhi_types.h"
#include "rhi/rhi_resources.h"

namespace Haruka { namespace Core { class Camera; } class PlanetarySystem; class FluidHost; }

namespace Haruka::World {

struct FluidBridge {
    struct Frame {
        PlanetarySystem* planets = nullptr;
        Core::Camera*    camera  = nullptr;
        float dt = 0.016f;
        float rainAmount = 0.0f;                 ///< 0..1 (SkyPass::weather()); se convierte a m/s de lamina
        int   renderW = 1, renderH = 1;          ///< del target de escena
        RHI::RenderPassHandle sceneTargetPass{}; ///< donde dibuja el fluido (con el depth de la escena)
        RHI::BufferHandle     perFrameUBO{};     ///< binding 0 (PerFrameData) para softbody/fluido
    };
    /// Cada frame, tras el planeta y la precipitacion. Crea el host la primera vez que hay agua.
    static void run(std::unique_ptr<FluidHost>& host, const Frame& f);
};

} // namespace Haruka::World
