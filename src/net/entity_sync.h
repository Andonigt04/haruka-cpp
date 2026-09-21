#pragma once
/**
 * @file net/entity_sync.h
 * @brief LAS ENTIDADES REMOTAS EN LA ESCENA: vacia la cola del cliente DGS, crea el objeto de
 *        escena de cada jugador/objeto del mundo que llega, lo mueve, y lo caduca por TTL (nada en
 *        el cable dice "se ha ido"). Vivia en `Application` (application_render.cpp); Andoni
 *        (21-09): "los modulos estan en application_*".
 */
#include <cstdint>
#include <map>
#include <unordered_set>

#include "include/dgs/client.h"

namespace Haruka { class SceneManager; }

namespace Haruka::Net {

class EntitySync {
public:
    /// Which uuid is US, so the world feed does not spawn a second copy of the local player: the
    /// zone broadcasts the sender's own entity back along with everyone else's.
    void setLocalUuid(uint32_t uuid) { m_localUuid = uuid; }
    /// Un objeto del mundo que ESTE cliente ya dibuja por su cuenta (lo pidio el): no se duplica.
    void adoptWorldObject(uint32_t uuid, SceneManager* scene);
    /// Cada frame de simulacion (antes de dibujar): la llaman el bucle del juego Y `renderFrame()`.
    void sync(DGS::Client& dgs, SceneManager* scene);

private:
    uint32_t m_localUuid = 0;                 // 0 = not set: nothing is filtered
    std::map<uint32_t, double> m_lastSeen;    // uuid → cuando se oyo por ultima vez (TTL)
    std::unordered_set<uint32_t> m_locallyOwnedWorld;
    static constexpr double kTtlS = 5.0;
};

} // namespace Haruka::Net
