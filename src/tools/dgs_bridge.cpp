#ifdef HARUKA_NETWORK

#include "tools/dgs_bridge.h"
#include "renderer/motor_instance.h"
#include "core/application.h"

namespace Haruka::Network {

void sendState(uint32_t uuid, const WorldPos& pos, const Rotation& rot,
               const uint8_t* data, uint16_t size) {
    auto* app = MotorInstance::getInstance().getApplication();
    if (!app) return;
    app->sendPlayerState(uuid, pos, rot, data, size);
}



void setLocalPlayerUuid(uint32_t uuid) {
    auto* app = MotorInstance::getInstance().getApplication();
    if (!app) return;
    app->setLocalPlayerUuid(uuid);
}

void sendChat(uint32_t uuid, const std::string& username, const std::string& text, uint8_t channel) {
    auto* app = MotorInstance::getInstance().getApplication();
    if (!app) return;
    app->sendPlayerChat(uuid, username, text, channel);
}

bool hasWorldTime() {
    auto* app = MotorInstance::getInstance().getApplication();
    return app && app->networkHasWorldTime();
}

double worldTimeSeconds() {
    auto* app = MotorInstance::getInstance().getApplication();
    return app ? app->networkWorldTimeSeconds() : 0.0;
}


void sendPlacePiece(uint32_t actor, uint16_t typeId,
                    const WorldPos& pos, const glm::dquat& orient) {
    auto* app = MotorInstance::getInstance().getApplication();
    if (!app) return;
    app->sendPlacePiece(actor, typeId, pos, orient);
}

uint32_t sendDropItem(uint32_t actor, const WorldPos& pos, const std::string& kind) {
    auto* app = MotorInstance::getInstance().getApplication();
    if (!app) return 0;
    return app->sendDropItem(actor, pos, kind);
}


std::vector<DGS::ActionAck> pollActionResults() {
    auto* app = MotorInstance::getInstance().getApplication();
    if (!app) return {};
    return app->pollActionResults();
}

void adoptWorldObject(uint32_t uuid) {
    if (auto* app = MotorInstance::getInstance().getApplication()) app->adoptWorldObject(uuid);
}

uint32_t sessionUuid() {
    auto* app = MotorInstance::getInstance().getApplication();
    return app ? app->networkSessionUuid() : 0u;
}

std::vector<DGS::ChatMessage> pollChats() {
    auto* app = MotorInstance::getInstance().getApplication();
    if (!app) return {};
    return app->pollPlayerChats();
}

}

#endif
