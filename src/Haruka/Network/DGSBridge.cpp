#ifdef HARUKA_NETWORK

#include "Haruka/Network/DGSBridge.h"
#include "Haruka/Renderer/MotorInstance.h"
#include "Haruka/Core/Application.h"

namespace Haruka::Network {

void sendState(uint32_t uuid, const Tools::WorldPos& pos, const Tools::Rotation& rot,
               const uint8_t* data, uint16_t size) {
    auto* app = Renderer::MotorInstance::getInstance().getApplication();
    if (!app) return;
    app->sendPlayerState(uuid, pos, rot, data, size);
}



void setLocalPlayerUuid(uint32_t uuid) {
    auto* app = Renderer::MotorInstance::getInstance().getApplication();
    if (!app) return;
    app->setLocalPlayerUuid(uuid);
}

void sendChat(uint32_t uuid, const std::string& username, const std::string& text, uint8_t channel) {
    auto* app = Renderer::MotorInstance::getInstance().getApplication();
    if (!app) return;
    app->sendPlayerChat(uuid, username, text, channel);
}

bool hasWorldTime() {
    auto* app = Renderer::MotorInstance::getInstance().getApplication();
    return app && app->networkHasWorldTime();
}

double worldTimeSeconds() {
    auto* app = Renderer::MotorInstance::getInstance().getApplication();
    return app ? app->networkWorldTimeSeconds() : 0.0;
}


void sendPlacePiece(uint32_t actor, uint16_t typeId,
                    const Tools::WorldPos& pos, const glm::dquat& orient) {
    auto* app = Renderer::MotorInstance::getInstance().getApplication();
    if (!app) return;
    app->sendPlacePiece(actor, typeId, pos, orient);
}

uint32_t sendDropItem(uint32_t actor, const Tools::WorldPos& pos, const std::string& kind) {
    auto* app = Renderer::MotorInstance::getInstance().getApplication();
    if (!app) return 0;
    return app->sendDropItem(actor, pos, kind);
}


std::vector<DGS::ActionAck> pollActionResults() {
    auto* app = Renderer::MotorInstance::getInstance().getApplication();
    if (!app) return {};
    return app->pollActionResults();
}

void adoptWorldObject(uint32_t uuid) {
    if (auto* app = Renderer::MotorInstance::getInstance().getApplication()) app->adoptWorldObject(uuid);
}

uint32_t sessionUuid() {
    auto* app = Renderer::MotorInstance::getInstance().getApplication();
    return app ? app->networkSessionUuid() : 0u;
}

std::vector<DGS::ChatMessage> pollChats() {
    auto* app = Renderer::MotorInstance::getInstance().getApplication();
    if (!app) return {};
    return app->pollPlayerChats();
}

}

#endif
