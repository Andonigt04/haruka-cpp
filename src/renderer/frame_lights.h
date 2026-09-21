#pragma once
/**
 * @file renderer/frame_lights.h
 * @brief LAS LUCES DEL FRAME: el sol (direccion y color del WorldSystem, multi-estrella), la luna y
 *        el ambiente que sale del CIELO QUE SE DIBUJA (armonicos esfericos de la misma paleta que
 *        sky.frag, recalculados por tramos de elevacion solar), repartidos al UBO por frame y al
 *        planeta. Y el diagnostico de los cuerpos emisores ("¿el sol no se renderiza?" son tres
 *        numeros: detras, fuera del frustum o subpixel). Vivia en `Application::passSceneSetup`.
 */
#include <glm/glm.hpp>

#include "renderer/scene_ubo.h"

namespace Haruka { namespace Core { class Camera; } class PlanetarySystem; class WorldSystem; class SceneManager; }

namespace Haruka::Renderer {

class FrameLights {
public:
    struct Frame {
        Core::Camera*    camera  = nullptr;
        WorldSystem*     world   = nullptr;
        PlanetarySystem* planets = nullptr;
        SceneManager*    scene   = nullptr;     ///< para el sync perezoso de los cuerpos del mundo
        glm::vec3        cameraOrigin{0.0f};
        int              heightPx = 1;          ///< para el tamaño en pixeles del diagnostico
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
