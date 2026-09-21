// Todo el fichero compila a nada sin red (como application_network.cpp): el include del cliente
// DGS solo existe con HARUKA_NETWORK.
#ifdef HARUKA_NETWORK
#include "net/entity_sync.h"

#include "core/logger.h"
#include "core/scene/scene_manager.h"
#include "tools/math_types.h"
#include "tools/object_types.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>

namespace Haruka::Net {

// ⚠️ ESTO VIVIA DENTRO DE `renderFrame()`, QUE EL JUEGO NO LLAMA NUNCA.
//
// El bucle principal (`Application::run`) llama a `renderFrameContent()`; `renderFrame()` es la ruta
// del EDITOR, que lo invoca desde fuera. Así que todo lo que sigue — vaciar la cola de entidades del
// cliente, crear el objeto de escena de cada jugador remoto, moverlo, y caducarlo — era codigo muerto
// en el juego. Sintoma: te conectas, el cliente RECIBE a los demas (medido con una sonda plantada en
// el punto exacto del spawn: 2 entidades), y en pantalla no hay nadie. Ni un error, ni un log.
//
// Peor que invisible: la cola de `pollEntities` es acotada (4096), asi que nadie la vaciaba y el
// mundo entrante se descartaba en silencio en cuanto se llenaba.
//
// Ahora es una funcion, y la llaman LAS DOS rutas: el bucle del juego antes de dibujar (es
// simulacion, no render) y `renderFrame()` para que el editor siga viendo lo mismo.
// ⚠️ LA DEFINICIÓN VA DENTRO DEL GUARDIA, como su declaración. Estaba fuera, con un `#ifdef` sólo en
// el cuerpo, y así el motor dejaba de compilar SIN red: "no declaration matches". No se vio en el
// juego porque el juego siempre define `HARUKA_NETWORK` — quien lo compila sin red es el EDITOR, y
// nadie lo había construido desde entonces.
void EntitySync::adoptWorldObject(uint32_t uuid, SceneManager* scene) {
    if (!uuid) return;
    m_locallyOwnedWorld.insert(uuid);
    // El acuse puede llegar DESPUÉS de la primera difusión que ya trae el objeto: la zona contesta a
    // quien pidió y a la vez lo mete en su reparto de 10 Hz, y cuál de los dos llega antes no lo
    // decide nadie. Si la copia ya está creada, se retira; si no, el filtro de arriba la evita.
    if (scene) scene->removeObject("entity_" + std::to_string(uuid));
    m_lastSeen.erase(uuid);
}

void EntitySync::sync(DGS::Client& dgs, SceneManager* scene) {
    if (scene) {
        // A clock of its own: the TTL below is about wall time since a player was last heard from,
        // which no frame counter answers.
        const double nowSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        for (const auto& transfer : dgs.pollEntities()) {
            // ⚠️ TWO THINGS THIS LOOP GOT WRONG, and neither could show while the client received
            // nothing. It does now (`Client::udpLoop`), so both are live:
            //
            //   · THE ZONE ECHOES YOU BACK. Its broadcast includes the sender's own entity, so without
            //     this the local player gets a second, remote-controlled copy of themselves standing
            //     wherever the server last saw them.
            //   · AND THIS ADDED A NEW OBJECT PER UPDATE. `addLoadedObject` does `push_back` on the
            //     scene's vector — the name registry is overwritten, the vector is not — so at 10 Hz
            //     with N players nearby the scene grew by 10*N objects a second, for ever. An entity
            //     that is already in the scene is MOVED, not added again.
            if (transfer.uuid == m_localUuid) continue;
            // Y lo que este cliente ya dibuja por su cuenta: lo que TÚ tiraste vuelve por el feed
            // como cualquier otro objeto del mundo, y crear aquí una segunda copia deja tu objeto
            // real cayendo debajo de otro idéntico que no se mueve. Ver `adoptWorldObject`.
            if (m_locallyOwnedWorld.count(transfer.uuid)) continue;

            const std::string name = "entity_" + std::to_string(transfer.uuid);
            const Haruka::WorldPos where(
                transfer.chunkX * Haruka::Units::KM + transfer.pos[0],
                transfer.chunkY * Haruka::Units::KM + transfer.pos[1],
                transfer.chunkZ * Haruka::Units::KM + transfer.pos[2]
            );
            // The wire carries yaw as a uint16 over the full turn, packed by `Client::sendTransform`.
            // Dropping it, which is what happened, left every remote player facing the same way.
            const double yawDeg = (double)transfer.angle * (360.0 / 65536.0) - 180.0;

            m_lastSeen[transfer.uuid] = nowSeconds;

            if (auto existing = scene->getObject(name)) {
                existing->position = where;
                existing->rotation.y = yawDeg;
                continue;
            }

            auto obj = std::make_shared<Haruka::SceneObject>();
            obj->name = name;
            obj->type = (transfer.type == DGS::ENT_PLAYER) ? "Character" :
                        (transfer.type == DGS::ENT_NPC)    ? "Character" : "Spacecraft";
            // ⚠️ WITHOUT THIS THEY ARE INVISIBLE. `classifySceneObject` dispatches on the `objectType`
            // ENUM; the string `type` above is a label. An object left at `UNKNOWN` with no mesh, no
            // model and no primitive property classifies as `RenderKind::None` — added to the scene,
            // moved every frame, and never drawn. Ghosts (a neighbouring zone's projection) were being
            // drawn all along, through a different path that attaches a capsule explicitly, so the
            // players you could see were the distant ones and the players you could not were the ones
            // standing next to you.
            obj->objectType = (transfer.type == DGS::ENT_PLAYER || transfer.type == DGS::ENT_NPC)
                              ? Haruka::ObjectType::CHARACTER : Haruka::ObjectType::MODEL;

            // ⚠️ UN OBJETO DEL MUNDO NO ES UN JUGADOR SIN MODELO. Lleva el bit STATE_WORLD_OWNED (lo
            // exime del GC de leases de la zona) y su payload dice QUE es — el otro cliente no puede
            // adivinar que una mesa es una mesa. Sin leerlo, todo lo que alguien coloque le aparece al
            // vecino como una capsula, que es el respaldo para lo que no tiene nada que dibujar.
            if (transfer.state & DGS::STATE_WORLD_OWNED) {
                obj->type = "Prop";
                obj->properties["placed"]    = true;
                obj->properties["fromWorld"] = true;   // no es tuyo: lo puso otro, o estaba ya ahi
                if (transfer.dataSize > 0) {
                    const std::string kind((const char*)transfer.data,
                                           std::min<size_t>(transfer.dataSize, sizeof(transfer.data)));
                    if (!kind.empty()) obj->modelPath = kind;
                }
                // El contador de abajo dice CUANTAS entidades hay, no QUE son. Un jugador y una mesa
                // que alguien dejo en el suelo suman igual, asi que "veo 2" no distingue haber visto
                // al vecino de haber visto lo que tiro. Una linea por objeto, solo al crearlo.
                HARUKA_LOGI("Net", "objeto del mundo %u: '%s'", transfer.uuid,
                            obj->modelPath.empty() ? "(sin modelo)" : obj->modelPath.c_str());
            }

            obj->position = where;
            obj->rotation.y = yawDeg;
            scene->addLoadedObject(obj);
        }

        // ⚠️ NOBODY EVER LEAVES OTHERWISE. There is no "this entity is gone" packet and there cannot
        // usefully be one: a player stops being yours because they walked out of your interest radius,
        // because their lease expired, or because they closed the game — and from here those are the
        // same silence. The viewer solves it with a TTL and so does this: an entity not heard from for
        // `kTtlS` is removed. Without it, walking past somebody leaves a statue behind for
        // the rest of the session.
        // ⚠️ "NO VEO A NADIE" TIENE TRES CAUSAS Y NINGUNA SE DISTINGUE MIRANDO LA PANTALLA: no
        // llegan (nadie cerca, o el radio de interes te deja fuera), llegan y no se crean, o se crean
        // y no se dibujan. Sin este contador hay que adivinar cual de las tres, y ya he adivinado mal
        // una vez hoy. Una linea cada dos segundos, solo cuando hay red y solo si algo cambia.
        {
            static double s_lastReport = 0.0;
            static size_t s_lastCount  = (size_t)-1;
            if (nowSeconds - s_lastReport > 2.0 && m_lastSeen.size() != s_lastCount) {
                s_lastReport = nowSeconds;
                s_lastCount  = m_lastSeen.size();
                size_t inScene = 0;
                for (const auto& kv : m_lastSeen)
                    if (scene->getObject("entity_" + std::to_string(kv.first))) ++inScene;
                HARUKA_LOGI("Net", "otras entidades: %zu recibidas · %zu en la escena%s",
                            m_lastSeen.size(), inScene,
                            m_lastSeen.empty() ? "  (nadie dentro de tu radio de interes)" : "");
            }
        }

        for (auto it = m_lastSeen.begin(); it != m_lastSeen.end(); ) {
            if (nowSeconds - it->second < kTtlS) { ++it; continue; }
            scene->removeObject("entity_" + std::to_string(it->first));
            it = m_lastSeen.erase(it);
        }
    }
}


} // namespace Haruka::Net
#endif // HARUKA_NETWORK
