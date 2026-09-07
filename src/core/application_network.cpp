// Application — DGS network bridge.
// Thin adapters between the engine's world-space transforms/chat and the DGS
// client (chunk-local quantisation, connection setup, status queries). Entire
// file compiles to nothing when networking is disabled.

#include "application.h"

#ifdef HARUKA_NETWORK
// PlaceAction / ActionHeader: el payload que el modulo de reglas lee detras de la cabecera.
#include "include/dgs/game_module.h"

#include <cmath>

namespace Haruka { namespace Core {

void Application::sendPlayerState(uint32_t uuid, const Haruka::WorldPos& pos, const Haruka::Rotation& rot,
                                  const uint8_t* data, uint16_t size) {
    if (!m_dgs.isConnected()) return;
    int32_t cx = (int32_t)std::floor(pos.x / Haruka::Units::KM);
    int32_t cy = (int32_t)std::floor(pos.y / Haruka::Units::KM);
    int32_t cz = (int32_t)std::floor(pos.z / Haruka::Units::KM);
    float localPos[3] = {
        (float)(pos.x - cx * Haruka::Units::KM),
        (float)(pos.y - cy * Haruka::Units::KM),
        (float)(pos.z - cz * Haruka::Units::KM)
    };
    float rotF[4] = { (float)rot.x, (float)rot.y, (float)rot.z, (float)rot.w };
    m_dgs.sendState(uuid, cx, cy, cz, localPos, rotF, data, size);
}



void Application::sendPlacePiece(uint32_t actor, uint16_t typeId,
                                const Haruka::WorldPos& pos, const glm::dquat& orient) {
    if (!m_dgs.isConnected()) return;

    // El payload que el modulo espera detras de la cabecera: `PlaceAction`. Posicion GLOBAL en
    // doubles y cuaternion, que es como el cliente valida su propia colocacion — el servidor corre
    // exactamente la misma comprobacion, no una reimplementacion parecida.
    DGS::PlaceAction pa{};
    pa.typeId  = typeId;
    pa.pos[0]  = pos.x; pa.pos[1] = pos.y; pa.pos[2] = pos.z;
    pa.quat[0] = orient.x; pa.quat[1] = orient.y; pa.quat[2] = orient.z; pa.quat[3] = orient.w;

    const int32_t cx = (int32_t)std::floor(pos.x / Haruka::Units::KM);
    const int32_t cy = (int32_t)std::floor(pos.y / Haruka::Units::KM);
    const int32_t cz = (int32_t)std::floor(pos.z / Haruka::Units::KM);
    const float local[3] = {
        (float)(pos.x - cx * Haruka::Units::KM),
        (float)(pos.y - cy * Haruka::Units::KM),
        (float)(pos.z - cz * Haruka::Units::KM)
    };

    m_dgs.sendAction(actor, DGS::ACTION_PLACE, /*target*/0, cx, cy, cz, local, 0,
                     (const uint8_t*)&pa, (uint16_t)sizeof(pa));
}

uint32_t Application::sendDropItem(uint32_t actor, const Haruka::WorldPos& pos, const std::string& kind) {
    if (!m_dgs.isConnected()) return 0;
    const int32_t cx = (int32_t)std::floor(pos.x / Haruka::Units::KM);
    const int32_t cy = (int32_t)std::floor(pos.y / Haruka::Units::KM);
    const int32_t cz = (int32_t)std::floor(pos.z / Haruka::Units::KM);
    const float local[3] = {
        (float)(pos.x - cx * Haruka::Units::KM),
        (float)(pos.y - cy * Haruka::Units::KM),
        (float)(pos.z - cz * Haruka::Units::KM)
    };
    // El payload es QUE es: el vecino no puede adivinar que una piedra es una piedra. Sin `PlaceAction`
    // porque no hay pieza que encajar — tirar no es construir.
    return m_dgs.sendAction(actor, DGS::ACTION_DROP, /*target*/0, cx, cy, cz, local, 0,
                            (const uint8_t*)kind.c_str(), (uint16_t)kind.size());
}


std::vector<DGS::ActionAck> Application::pollActionResults() {
    return m_dgs.pollActionResults();
}

uint32_t Application::networkSessionUuid() const {
    return m_dgs.sessionId();
}

// ⚠️ AQUI HABIA `sendPlayerStats` Y `sendPlayerInventory`, Y ERAN LA MISMA PUERTA QUE ESTA.
// Las tres construian el mismo `EntityTransfer` con un campo distinto relleno, y separarlas obligaba
// al cliente a recordar lo que las otras habian dicho — de ahi salieron dos bugs: unos `Stats` en
// blanco que borraban la velocidad declarada, y una posicion a cero que teletransportaba al jugador
// al origen de su chunk cada vez que declaraba su vida.
//
// Ademas la velocidad ya no la decide el cliente: la pone la zona (`client_claims_e2e`), asi que
// `sendPlayerStats` no tenia ni efecto. Y la vida, el dano y el inventario son cosas del JUEGO —
// cambian de un juego a otro y no tienen por que parecerse a `DGS::Stats`—, asi que viajan en el
// payload opaco de `sendPlayerState`, que es para lo que existe.

bool Application::networkHasWorldTime() const {
    return m_dgs.hasWorldTime();
}

double Application::networkWorldTimeSeconds() const {
    return m_dgs.worldTimeSeconds();
}


void Application::sendPlayerChat(uint32_t uuid, const std::string& username, const std::string& text,
                                 uint8_t channel) {
    if (!m_dgs.isConnected()) return;
    m_dgs.sendChat(uuid, username, text, channel);
}

std::vector<DGS::ChatMessage> Application::pollPlayerChats() {
    return m_dgs.pollChats();
}

bool Application::connectDGS(const std::string& headHost, int headPort,
                               const std::string& email,    const std::string& password,
                               const std::string& apiHost,  int apiPort) {
    return m_dgs.connect(headHost, headPort, email, password,
                         apiHost.empty() ? headHost : apiHost,
                         apiPort <= 0   ? headPort + 1 : apiPort);
}

bool Application::isNetworkConnected() const { return m_dgs.isConnected(); }
int  Application::getGhostCount()      const { return (int)m_ghostObjects.size(); }

}} // namespace Haruka::Core

#endif // HARUKA_NETWORK
