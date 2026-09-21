#pragma once
/**
 * @file renderer/shadow_pass.h
 * @brief EL MAPA DE SOMBRAS DEL SOL y LA MASCARA CENITAL como modulo. La sombra: ortografica ajustada
 *        alrededor del jugador (±42 m), los casters del juego (`onRenderShadow`) + los props del
 *        motor. La mascara: el mismo truco con la "luz" en el CENIT — lo que aparece es lo que tienes
 *        ENCIMA, que es lo que el depth no puede decir: nada de lluvia bajo cubierto, silueta seca
 *        bajo cualquier collider, donde cuaja la nieve. Solo cuando precipita o el suelo esta mojado.
 *        Vivia en `Application::passShadows` (mas `_shadow` y la mascara cenital).
 */
#include <memory>
#include <glm/glm.hpp>

#include "core/game_interface.h"   // OnRenderShadowFunc
#include "renderer/shadow.h"
#include "rhi/rhi_types.h"
#include "rhi/rhi_resources.h"

namespace Haruka { namespace Core { class Camera; } class PlanetarySystem; namespace World { class PropSystem; }
                   namespace RHI { class Context; } }

namespace Haruka::Renderer {

class ShadowPass {
public:
    /// Lo del clima que decide si hace falta la mascara cenital (copia de SkyPass::weather()).
    struct WeatherView { float rainAmount = 0, snowAmount = 0, groundWetness = 0, snowAccum = 0; };
    struct Frame {
        Core::Camera*      camera  = nullptr;
        PlanetarySystem*   planets = nullptr;
        World::PropSystem* props   = nullptr;
        Haruka::GameInterface::OnRenderShadowFunc onRenderShadow = nullptr;   ///< los casters del JUEGO
        glm::vec3 sunDirection{0.0f, 1.0f, 0.0f};
        const WeatherView* weather = nullptr;
    };

    /// Dentro del frame, con el pase de escena ya cerrado: abre y cierra sus propios render passes,
    /// y reparte al planeta el mojado + la mascara (`setGroundWet`).
    void draw(RHI::Context* ctx, const Frame& f);

    bool shadowReady() const { return m_shadow != nullptr; }   ///< para `enableShadows` del UBO
    bool skyMaskOn() const { return m_skyMaskOn; }
    const glm::mat4& skySpace() const { return m_skySpace; }
    RHI::TextureHandle skyMaskTexture() const;   ///< invalido si no se rellena este frame

private:
    std::unique_ptr<Shadow> m_shadow;    ///< mapa de sombras del sol (resolucion por ajuste)
    std::unique_ptr<Shadow> m_skyMask;   ///< mascara cenital (depth desde el cenit)
    glm::mat4 m_lightSpace{1.0f};
    glm::mat4 m_skySpace{1.0f};
    bool m_shadowsOn = false, m_skyMaskOn = false;
    static constexpr unsigned kSkyMaskRes     = 1024u;
    static constexpr double   kSkyMaskExtentM = 48.0;
};

} // namespace Haruka::Renderer
