// Application — DGS network bridge.
// Thin adapters between the engine's world-space transforms/chat and the DGS
// client (chunk-local quantisation, connection setup, status queries). Entire
// file compiles to nothing when networking is disabled.

#include "application.h"

#ifdef HARUKA_NETWORK

#include <cmath>

namespace Haruka { namespace Core {

void Application::sendPlayerTransform(uint32_t uuid, const Haruka::WorldPos& pos, const Haruka::Rotation& rot) {
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
    m_dgs.sendTransform(uuid, cx, cy, cz, localPos, rotF);
}

void Application::sendPlayerChat(uint32_t uuid, const std::string& username, const std::string& text) {
    if (!m_dgs.isConnected()) return;
    m_dgs.sendChat(uuid, username, text);
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
