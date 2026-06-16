#pragma once
#ifdef HARUKA_NETWORK

#include "tools/math_types.h"
#include "include/dgs/types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Haruka::Network {
    void sendTransform(uint32_t uuid, const WorldPos& pos, const Rotation& rot);
    void sendChat(uint32_t uuid, const std::string& username, const std::string& text);
    
    std::vector<DGS::ChatMessage> pollChats();
}

#endif
