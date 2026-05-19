#ifdef HARUKA_NETWORK

#include "tools/dgs_bridge.h"
#include "renderer/motor_instance.h"
#include "core/application.h"

namespace Haruka::Network {

void sendTransform(uint32_t uuid, const WorldPos& pos, const Rotation& rot) {
    auto* app = MotorInstance::getInstance().getApplication();
    if (!app) return;
    app->sendPlayerTransform(uuid, pos, rot);
}

void sendChat(uint32_t uuid, const std::string& username, const std::string& text) {
    auto* app = MotorInstance::getInstance().getApplication();
    if (!app) return;
    app->sendPlayerChat(uuid, username, text);
}

std::vector<DGS::ChatMessage> pollChats() {
    auto* app = MotorInstance::getInstance().getApplication();
    if (!app) return {};
    return app->pollPlayerChats();
}

}

#endif
