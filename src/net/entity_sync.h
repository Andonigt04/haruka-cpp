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
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "include/dgs/client.h"

namespace Haruka { class SceneManager; }

namespace Haruka::Net {

/// One received snapshot of a remote entity's transform. World coordinates (chunk*KM + local) so a
/// chunk border never makes the path discontinuous; yaw in radians.
struct Sample { double t, x, y, z, yaw; };

class EntitySync {
public:
    /// Which uuid is US, so the world feed does not spawn a second copy of the local player: the
    /// zone broadcasts the sender's own entity back along with everyone else's.
    void setLocalUuid(uint32_t uuid) { m_localUuid = uuid; }
    /// Un objeto del mundo que ESTE cliente ya dibuja por su cuenta (lo pidio el): no se duplica.
    void adoptWorldObject(uint32_t uuid, SceneManager* scene);
    /// Cada frame de simulacion (antes de dibujar): la llaman el bucle del juego Y `renderFrame()`.
    void sync(DGS::Client& dgs, SceneManager* scene);
    /// Lo que hay AHORA en la escena venido del feed, por tipo (para el panel de depuracion).
    struct Counts { int players = 0, npcs = 0, worldObjects = 0; };
    Counts counts() const;

    /// ⚠️ "¿LA ZONA ME OYE?" — la unica prueba, y no era visible por ninguna parte. El panel cuenta
    /// "jugadores" sin incluirte a ti (tu uuid se filtra abajo), asi que un contador a 0 no distingue
    /// "no hay nadie mas" de "mi transform no llega a ninguna zona" (familias de direcciones, chunk
    /// sin cubrir, radio de interes…). La zona SI te devuelve tu propio eco por su broadcast: esto
    /// anota CUANDO pasó, y el panel lo enseña como "zona te oye: si (eco hace Xs)".
    /// @return edad del ultimo eco en segundos, o -1 si la zona todavia no te ha devuelto nada.
    double selfEchoAgeS() const;

private:
    uint32_t m_localUuid = 0;                 // 0 = not set: nothing is filtered
    std::map<uint32_t, double> m_lastSeen;    // uuid → cuando se oyo por ultima vez (TTL)
    std::map<uint32_t, uint8_t> m_kind;       // uuid → 0 jugador · 1 npc · 2 objeto del mundo
    std::unordered_set<uint32_t> m_locallyOwnedWorld;
    double m_lastHeardSelfAt = 0.0;           // wall (steady) s del ultimo eco propio; 0 = nunca
    static constexpr double kTtlS = 5.0;
    std::unordered_map<uint32_t, std::vector<Sample>> m_hist;   // uuid → historia de snapshots

    void pushSample(uint32_t uuid, double t, double x, double y, double z, double yawRad);
    void interpolateAll(SceneManager* scene, double nowSeconds);
};

} // namespace Haruka::Net
