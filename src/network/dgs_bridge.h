#pragma once
#ifdef HARUKA_NETWORK

#include "tools/math_types.h"
#include <cstdint>

namespace Haruka::Network {
    // Sends local player transform to the DGS zone server.
    // Chunk conversion and DGS internals are handled by the engine — callers only provide world-space data.
    void sendTransform(uint32_t uuid, const WorldPos& pos, const Rotation& rot);
}

#endif
