#pragma once
/**
 * @file Renderer/FrameLights.h
 * @brief LAS LUCES DEL FRAME, repartidas: el Sol, la Luna y el ambiente que calcula `SkyLight` por
 *        fisica, llevados al UBO por frame (los shaders que aun leen `sunLightColor`) y al planeta. Y el diagnostico de los cuerpos emisores ("¿el sol no se renderiza?" son tres
 *        numeros: detras, fuera del frustum o subpixel). Vivia en `Application::passSceneSetup`.
 */
#include <glm/glm.hpp>

#include "Haruka/Renderer/SceneUBO.h"
#include "Haruka/Renderer/SkyLight.h"

namespace Haruka { namespace Core { class Camera; } }
namespace Haruka::World::Planet { class PlanetarySystem; }
namespace Haruka::World { class WorldSystem; }
namespace Haruka::Core::Scene { class SceneManager; }

namespace Haruka::Renderer {

class FrameLights {
public:
    struct Frame {
        Core::Camera*    camera  = nullptr;
        World::WorldSystem*     world   = nullptr;
        World::Planet::PlanetarySystem* planets = nullptr;
        Core::Scene::SceneManager*    scene   = nullptr;     ///< para el sync perezoso de los cuerpos del mundo
        glm::vec3        cameraOrigin{0.0f};
        int              heightPx = 1;          ///< para el tamaño en pixeles del diagnostico
        const SkyLightUBO* sky = nullptr;       ///< la luz del frame por fisica (Renderer/SkyLight.h)
        double           diagClock = 0.0;
    };
    /// Rellena sol, luna y ambiente en `fd`, y se los pasa al planeta (`setSunLight`, `setSkyAmbientSH`).
    void fill(PerFrameUBOData& fd, const Frame& f);
    /// Cada 5 s (HARUKA_DIAG): donde cae cada estrella en pantalla y cuanto mide.
    void starDiagnostics(const PerFrameUBOData& fd, const Frame& f);

private:
    double m_lastStarLog = -1e9;
};

} // namespace Haruka::Renderer
