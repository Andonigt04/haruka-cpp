#include "world_system.h"
#include "camera.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace Haruka {

WorldSystem::WorldSystem() : worldOrigin(0.0, 0.0, 0.0) {}

WorldSystem::~WorldSystem() {}

Haruka::LocalPos WorldSystem::toLocal(Haruka::WorldPos objectPos, Haruka::WorldPos referencePos) {
    glm::dvec3 diff = objectPos - referencePos;
    return Haruka::LocalPos(diff);
}

void WorldSystem::updateOrigin(Haruka::WorldPos newOrigin) {
    glm::dvec3 offset = newOrigin - worldOrigin;
    worldOrigin = newOrigin;

    for (auto& body : celestialBodies) {
        body.worldPos -= offset;
    }
}

void WorldSystem::updateLocalPositions(Haruka::WorldPos cameraWorldPos) {
    for (auto& body : celestialBodies) {
        body.localPos = toLocal(body.worldPos, cameraWorldPos);
    }
}

void WorldSystem::addBody(const CelestialBody& body) {
    celestialBodies.push_back(body);
}

void WorldSystem::removeBody(const std::string& name) {
    celestialBodies.erase(
        std::remove_if(celestialBodies.begin(), celestialBodies.end(),
            [&name](const CelestialBody& b) { return b.name == name; }),
        celestialBodies.end()
    );
}

CelestialBody* WorldSystem::findBody(const std::string& name) {
    for (auto& body : celestialBodies) {
        if (body.name == name) return &body;
    }
    return nullptr;
}

const std::vector<CelestialBody>& WorldSystem::getBodies() const {
    return celestialBodies;
}

size_t WorldSystem::getBodyCount() const {
    return celestialBodies.size();
}

Haruka::WorldPos WorldSystem::getOrigin() const {
    return worldOrigin;
}

void WorldSystem::markChunkResident(const PlanetChunkKey& key, bool resident) {
    auto& st = chunkStates[key];
    if (resident && !st.resident) {
        residentMemoryBytes += st.bytesUsed;
    } else if (!resident && st.resident) {
        residentMemoryBytes = (st.bytesUsed <= residentMemoryBytes) ? (residentMemoryBytes - st.bytesUsed) : 0;
    }
    st.resident = resident;
    if (resident) st.lastTouchedFrame = chunkFrameCounter;
}

size_t WorldSystem::getResidentChunkCount() const {
    size_t count = 0;
    for (const auto& [_, st] : chunkStates) {
        if (st.resident) count++;
    }
    return count;
}

void WorldSystem::setChunkSize(const PlanetChunkKey& key, uint32_t bytes) {
    auto it = chunkStates.find(key);
    if (it == chunkStates.end()) return;
    if (it->second.resident) {
        if (bytes >= it->second.bytesUsed) {
            residentMemoryBytes += static_cast<size_t>(bytes - it->second.bytesUsed);
        } else {
            residentMemoryBytes -= static_cast<size_t>(it->second.bytesUsed - bytes);
        }
    }
    it->second.bytesUsed = bytes;
}

bool WorldSystem::updateBody(const CelestialBody& updatedBody) {
    // Find the body by name
    CelestialBody* existingBody = findBody(updatedBody.name);
    if (!existingBody) {
        return false;  // Body not found
    }
    
    // Update atomically
    *existingBody = updatedBody;
    
    // Notify all subscribers of the update
    notifyBodyUpdated(updatedBody.name);
    
    return true;
}

void WorldSystem::registerSystem(SystemSubscriber* subscriber) {
    if (!subscriber) return;
    
    // Avoid duplicate registration
    auto it = std::find(subscribers.begin(), subscribers.end(), subscriber);
    if (it == subscribers.end()) {
        subscribers.push_back(subscriber);
    }
}

void WorldSystem::unregisterSystem(SystemSubscriber* subscriber) {
    if (!subscriber) return;
    
    auto it = std::find(subscribers.begin(), subscribers.end(), subscriber);
    if (it != subscribers.end()) {
        subscribers.erase(it);
    }
}

void WorldSystem::notifyBodyUpdated(const std::string& bodyName) {
    for (auto subscriber : subscribers) {
        if (subscriber) {
            subscriber->onBodyUpdated(bodyName);
        }
    }
}

void WorldSystem::notifyChunkLoaded(const PlanetChunkKey& chunkKey) {
    for (auto subscriber : subscribers) {
        if (subscriber) {
            subscriber->onChunkLoaded(chunkKey);
        }
    }
}

void WorldSystem::notifyChunkEvicted(const PlanetChunkKey& chunkKey) {
    for (auto subscriber : subscribers) {
        if (subscriber) {
            subscriber->onChunkEvicted(chunkKey);
        }
    }
}

} // namespace Haruka