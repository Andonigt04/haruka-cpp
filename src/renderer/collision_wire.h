#pragma once
/**
 * @file renderer/collision_wire.h
 * @brief ALAMBRE DE LA MALLA DE COLISION encima del terreno dibujado (herramienta de diagnostico).
 *
 * La disparidad del terreno se veia a ojo y no la detectaba ninguna medida: la sonda de paridad da
 * 2 cm, los pies quedan a centimetros del suelo, y el clipmap dibuja con quads de 4 m — la misma
 * reticula que colisiona. Con todo coherente y el problema visible, la salida es dibujar la malla
 * que la fisica TIENE de verdad sobre lo que el render pinta: si el alambre flota o se hunde, ya no
 * es una impresion. Ademas dibuja las cajas/conos/mallas de los props que hay en Jolt, con la MISMA
 * geometria que usa la fisica (copia CPU de `PropSystem`).
 *
 * Se enciende desde el juego/editor (`setEnabled`) o con `HARUKA_COLLISION_WIRE=1`. Vivia en
 * `Application` (~380 lineas); Andoni (21-09): "los modulos estan en application_*".
 */
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

#include "rhi/rhi_types.h"
#include "rhi/rhi_resources.h"

namespace Haruka { namespace Core { class Camera; } class PlanetarySystem;
                   namespace Physics { class PhysicsEngine; } namespace RHI { class Context; } }

namespace Haruka::Renderer {

class CollisionWire {
public:
    struct Frame {
        Core::Camera*            camera  = nullptr;
        PlanetarySystem*         planets = nullptr;
        Physics::PhysicsEngine*  physics = nullptr;
        /// Mallas de colision de los props por (prototipo → parte) y su copia CPU (PropSystem).
        const std::unordered_map<int, std::vector<int>>*                    meshShapes = nullptr;
        const std::unordered_map<int, std::vector<std::vector<glm::vec3>>>* meshCpu    = nullptr;
    };

    ~CollisionWire();
    void shutdown();

    void setEnabled(bool on, Physics::PhysicsEngine* physics);
    bool enabled() const { return m_on; }
    /// Dentro del pase de escena, tras el terreno. `viewProjRotOnly` = proyeccion × rotacion de la
    /// vista (la geometria es camara-relativa).
    void draw(RHI::Context* ctx, const glm::mat4& viewProjRotOnly, const Frame& f);

private:
    bool m_on = false;
    RHI::PipelineHandle m_linePSO{};
    RHI::BufferHandle   m_lineVB{}, m_lineUBO{};
    uint32_t m_lineVerts = 0;
    uint64_t m_lineRev   = ~0ull;   ///< revision de la malla del suelo con la que se subio el alambre
    uint64_t m_propVer   = ~0ull;   ///< version de estaticos (los OBB de props cambian mas a menudo)
};

} // namespace Haruka::Renderer
