#ifdef HARUKA_NETWORK

#include "network/dgs_bridge.h"
#include "renderer/motor_instance.h"
#include "core/application.h"

namespace Haruka::Network {

void sendTransform(uint32_t uuid, const WorldPos& pos, const Rotation& rot) {
    auto* app = MotorInstance::getInstance().getApplication();
    if (!app) return;
    app->sendPlayerTransform(uuid, pos, rot);
}

}

#endif
