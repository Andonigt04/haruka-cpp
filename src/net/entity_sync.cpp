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
#include <cmath>
#include <memory>
#include <string>

namespace Haruka::Net {

namespace {

constexpr double kPi         = 3.14159265358979323846;
// Apuntar a `now - retardo` convierte una serie de snapshots a ~10 Hz en movimiento continuo a ritmo
// de render. Retardo fijo -> no mezcla pasados distintos; extrapolacion corta y ACOTADA, para que al
// quedarse el feed un instante la entidad no se congele en seco ni despegue.
constexpr double kInterpDelayS = 0.12;
constexpr double kExtrapS      = 0.10;
constexpr double kHistKeepS    = kInterpDelayS + kExtrapS + 0.25;   // mas viejo que esto no sirve

double wrapPi(double a)
{
    while (a >  kPi) a -= 2.0 * kPi;
    while (a < -kPi) a += 2.0 * kPi;
    return a;
}

/// Muestrea la trayectoria de una entidad en `target` segundos de reloj: interpolacion entre los dos
/// snapshots que lo flanquean (lo normal), clavada al primero antes de que haya historial, y
/// extrapolacion por velocidad (diferencia entre los dos ultimos, acotada a `kExtrapS`) por detras
/// del ultimo. Devuelve false solo si no hay historial; si no hay datos para `target`, clava algo.
bool sampleAt(const std::vector<Sample>& v, double target,
              double& x, double& y, double& z, double& yaw)
{
    if (v.empty()) return false;
    const Sample& first = v.front();
    const Sample& last  = v.back();

    if (target <= first.t) { x = first.x; y = first.y; z = first.z; yaw = first.yaw; return true; }

    if (target >= last.t)
    {
        if (v.size() >= 2)
        {
            const Sample& prev = v[v.size() - 2];
            const double  dt   = last.t - prev.t;
            if (dt > 1e-5)
            {
                const double f = std::min(target - last.t, kExtrapS) / dt;
                x = last.x + (last.x - prev.x) * f;
                y = last.y + (last.y - prev.y) * f;
                z = last.z + (last.z - prev.z) * f;
                yaw = last.yaw + wrapPi(last.yaw - prev.yaw) * f;
                return true;
            }
        }
        x = last.x; y = last.y; z = last.z; yaw = last.yaw;
        return true;
    }

    for (size_t i = 0; i + 1 < v.size(); ++i)
    {
        if (target < v[i + 1].t)   // primera pareja con v[i].t <= target < v[i+1].t
        {
            const Sample& a = v[i];
            const Sample& b = v[i + 1];
            const double  f = (b.t - a.t > 1e-9) ? (target - a.t) / (b.t - a.t) : 0.0;
            x = a.x + (b.x - a.x) * f;
            y = a.y + (b.y - a.y) * f;
            z = a.z + (b.z - a.z) * f;
            yaw = a.yaw + wrapPi(b.yaw - a.yaw) * f;
            return true;
        }
    }
    return false;
}

} // namespace

void EntitySync::pushSample(uint32_t uuid, double t, double x, double y, double z, double yawRad)
{
    auto& v = m_hist[uuid];
    if (!v.empty() && t <= v.back().t) return;   // fuera de orden: no romper la serie
    v.push_back({t, x, y, z, yawRad});
    if (v.size() > 8) v.erase(v.begin());
}

void EntitySync::interpolateAll(SceneManager* scene, double nowSeconds)
{
    for (auto hit = m_hist.begin(); hit != m_hist.end(); )
    {
        auto& v = hit->second;
        while (!v.empty() && v.front().t < nowSeconds - kHistKeepS) v.erase(v.begin());
        if (v.empty()) { hit = m_hist.erase(hit); continue; }   // el TTL lo retiro arriba/abajo

        const double target = nowSeconds - kInterpDelayS;
        double x, y, z, yaw;
        if (sampleAt(v, target, x, y, z, yaw))
        {
            const std::string name = "entity_" + std::to_string(hit->first);
            if (auto o = scene->getObject(name))
            {
                o->position    = Haruka::WorldPos(x, y, z);
                o->rotation.y  = yaw * (180.0 / kPi);
            }
        }
        ++hit;
    }
}

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
    m_kind.erase(uuid);
    m_hist.erase(uuid);
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
            if (transfer.uuid == m_localUuid)
            {
                if (m_localUuid != 0) m_lastHeardSelfAt = nowSeconds;   // la zona devuelve tu eco
                continue;
            }
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
            m_kind[transfer.uuid] = (transfer.state & DGS::STATE_WORLD_OWNED) ? 2
                                  : (transfer.type == DGS::ENT_NPC) ? 1 : 0;

            // ⚠️ EL OBJETO YA NO SE DESPLAZA AQUI. Antes, cada snapshot (~10 Hz de egress) hacia
            // `existing->position = where`: salto directo a la ultima posicion = el "a tirones" con un
            // server remoto. Ahora el snapshot va al historial y la posicion la pone POR FRAME
            // `interpolateAll` (ritmo de render, retardo fijo de `kInterpDelayS` contra la cola).
            // Un objeto ya creado se mueve SOLO desde ahi.
            pushSample(transfer.uuid, nowSeconds,
                       where.x, where.y, where.z, yawDeg * (kPi / 180.0));
            if (scene->getObject(name)) continue;

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
            m_kind.erase(it->first);
            m_hist.erase(it->first);
            it = m_lastSeen.erase(it);
        }

        // EL MOVIMIENTO REAL, EN EL FRAME NO EN EL PAQUETE: al margen de si llego snapshot este
        // frame, todo ente con historial se coloca a `now - kInterpDelayS` interpolando/extrapolando.
        interpolateAll(scene, nowSeconds);
    }
}


EntitySync::Counts EntitySync::counts() const {
    Counts c;
    for (const auto& [uuid, k] : m_kind) { if (k == 0) ++c.players; else if (k == 1) ++c.npcs; else ++c.worldObjects; }
    return c;
}

double EntitySync::selfEchoAgeS() const {
    if (m_lastHeardSelfAt <= 0.0) return -1.0;   // la zona aun no te ha devuelto tu propio eco
    const double nowSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return nowSeconds - m_lastHeardSelfAt;
}

} // namespace Haruka::Net
#endif // HARUKA_NETWORK
