#pragma once
#ifdef HARUKA_NETWORK

#include "tools/math_types.h"
#include "include/dgs/types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Haruka::Network {
    /// LO QUE ESTE JUGADOR DICE DE SI MISMO, en una sola llamada.
    ///
    /// ⚠️ ERAN TRES —`sendTransform`, `sendStats`, `sendInventory`— y las tres mandaban el MISMO
    /// paquete con un campo distinto relleno. `data` es el payload del juego: vida, inventario, lo que
    /// cada juego tenga. `size = 0` es "sin cambios", no "vacio".
    void sendState(uint32_t uuid, const WorldPos& pos, const Rotation& rot,
                   const uint8_t* data = nullptr, uint16_t size = 0);





    /// Tells the engine which uuid is the local player, so the world feed does not spawn a second copy
    /// of them: the zone broadcasts the sender's own entity back along with everyone else's.
    void setLocalPlayerUuid(uint32_t uuid);
    /// `channel` picks the route: proximity through the zone, everything else through the social node.
    void sendChat(uint32_t uuid, const std::string& username, const std::string& text,
                  uint8_t channel = DGS::CHAT_GLOBAL);
    
    std::vector<DGS::ChatMessage> pollChats();

    /// THE WORLD'S CLOCK, if the cluster handed one out.
    ///
    /// The engine's simulation time starts at zero and accumulates the local frame delta, and every
    /// visible consequence of time is an analytic function of it — `orbitPositionAt(orbit, t)` places
    /// the planets, which is what makes the sun rise; the weather fronts move on the same number, and
    /// so does the tide. Two players who launched five minutes apart were therefore five minutes apart
    /// in the world: one at noon, one at midnight, in different weather.
    ///
    /// `false` means nobody handed out a clock; the caller must keep its own rather than pretend.
    bool   hasWorldTime();
    double worldTimeSeconds();

    /// El uuid que el cluster ha dado a ESTA sesion, o 0 si no hay red. Un juego debe identificarse
    /// con esto y no con una constante: dos clientes con el mismo uuid son la misma entidad y cada uno
    /// descarta al otro como su propio eco.
    uint32_t sessionUuid();

    /// Que ha decidido el servidor sobre lo que pediste. Vacio si no hay red o no ha llegado nada.
    /// Un juego coloca al instante —prediccion optimista, y hace bien— pero tiene que poder deshacerlo.
    std::vector<DGS::ActionAck> pollActionResults();

    /// "Ese objeto del mundo ya lo dibujo yo": evita la copia duplicada y sin física de lo que
    /// tú mismo has tirado. Se llama con el uuid del acuse. Ver `Application::adoptWorldObject`.
    void adoptWorldObject(uint32_t uuid);

    /// PIDE colocar una pieza. No la coloca: lo pide.
    ///
    /// El servidor decide con la MISMA geometria que el cliente (`Construction::validate`: que no
    /// solape otra pieza, que este anclada, que el apoyo sea el correcto) y solo entonces crea la
    /// entidad. Por eso viaja `typeId` y no un modelo: sin saber lo GRANDE que es la pieza no hay
    /// solape que comprobar, y el catalogo es lo que le da ese tamano al servidor.
    ///
    /// Sin modulo de reglas en el validador esto se RECHAZA, y esa es la conducta correcta.
    void sendPlacePiece(uint32_t actor, uint16_t typeId,
                        const WorldPos& pos, const glm::dquat& orient);

    /// PIDE tirar un objeto: sale de tu inventario y se convierte en objeto del MUNDO — lo ven los
    /// demas y sobrevive al reinicio. `kind` es su modelo, para que el vecino sepa que es.
    ///
    /// No es `sendPlacePiece`: colocar una pieza de construccion se valida con geometria (que no
    /// solape, que apoye) y tirar una piedra no tiene ni catalogo ni encaje.
    /// @return el `requestId` con el que reconocer el veredicto, o 0 si no se pudo pedir.
    uint32_t sendDropItem(uint32_t actor, const WorldPos& pos, const std::string& kind);

    /// PIDE tirar un objeto: sale de tu inventario y se convierte en objeto del MUNDO — lo ven los
    /// demas y sobrevive al reinicio. `kind` es su modelo, para que el vecino sepa que es.
    ///
    /// No es `sendPlacePiece`: colocar una pieza de construccion se valida con geometria (que no
    /// solape, que apoye) y tirar una piedra no tiene ni catalogo ni encaje.



}

#endif
