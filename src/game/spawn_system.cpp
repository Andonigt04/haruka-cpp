#include "spawn_system.h"
#include <iostream>
#include <algorithm>
#include <chrono>
#include <random>

namespace Haruka {

SpawnSystem::SpawnSystem() {
    std::cout << "[SpawnSystem] Initialized" << std::endl;
}

SpawnSystem::~SpawnSystem() {}

void SpawnSystem::registerSpawnPoint(const SpawnPoint& point) {
    spawnPoints[point.id] = point;
    std::cout << "[SpawnSystem] Registered spawn point: " << point.id 
              << " (type: " << (int)point.type << ", zone: " << point.zoneId << ")" << std::endl;
}

void SpawnSystem::removeSpawnPoint(const std::string& id) {
    spawnPoints.erase(id);
    std::cout << "[SpawnSystem] Removed spawn point: " << id << std::endl;
}

SpawnPoint* SpawnSystem::getSpawnPoint(const std::string& id) {
    auto it = spawnPoints.find(id);
    return (it != spawnPoints.end()) ? &it->second : nullptr;
}

std::vector<SpawnPoint> SpawnSystem::getSpawnPointsByType(SpawnType type, const std::string& zoneId) {
    std::vector<SpawnPoint> result;
    
    for (auto& [id, point] : spawnPoints) {
        if (point.type == type && point.active) {
            if (zoneId.empty() || point.zoneId == zoneId) {
                result.push_back(point);
            }
        }
    }
    
    return result;
}

glm::dvec3 SpawnSystem::getPlayerSpawnPosition(const std::string& userId, const std::string& zoneId) {
    // Check si tiene checkpoint
    auto checkpointIt = playerCheckpoints.find(userId);
    if (checkpointIt != playerCheckpoints.end()) {
        auto* checkpoint = getSpawnPoint(checkpointIt->second);
        if (checkpoint && checkpoint->zoneId == zoneId) {
            std::cout << "[SpawnSystem] Spawning player at checkpoint: " << checkpoint->id << std::endl;
            return getRandomPositionInRadius(checkpoint->position, checkpoint->radius);
        }
    }
    
    // Check si está en respawn
    auto respawnIt = pendingRespawns.find(userId);
    if (respawnIt != pendingRespawns.end() && respawnIt->second.canRespawn) {
        return getRespawnPosition(userId);
    }
    
    // Spawn inicial
    return getRandomSpawnPosition(SpawnType::PLAYER_START, zoneId);
}

glm::dvec3 SpawnSystem::getRandomSpawnPosition(SpawnType type, const std::string& zoneId) {
    auto* spawn = findBestSpawnPoint(type, zoneId);
    
    if (spawn) {
        spawn->currentEntities++;
        spawn->lastSpawn = std::chrono::system_clock::now().time_since_epoch().count();
        
        return getRandomPositionInRadius(spawn->position, spawn->radius);
    }
    
    // Fallback
    std::cout << "[SpawnSystem] No spawn point found, using default" << std::endl;
    return glm::dvec3(0, 10, 0);
}

void SpawnSystem::registerPlayerDeath(const std::string& userId, const glm::dvec3& position, const std::string& zoneId) {
    RespawnData data;
    data.userId = userId;
    data.deathPosition = position;
    data.zoneId = zoneId;
    data.deathTime = std::chrono::system_clock::now().time_since_epoch().count();
    data.respawnTimer = defaultRespawnTime;
    data.canRespawn = false;
    
    pendingRespawns[userId] = data;
    
    std::cout << "[SpawnSystem] Player died: " << userId << " (respawn in " << defaultRespawnTime << "s)" << std::endl;
}

bool SpawnSystem::canPlayerRespawn(const std::string& userId) {
    auto it = pendingRespawns.find(userId);
    return (it != pendingRespawns.end() && it->second.canRespawn);
}

glm::dvec3 SpawnSystem::getRespawnPosition(const std::string& userId) {
    auto it = pendingRespawns.find(userId);
    if (it == pendingRespawns.end()) {
        return glm::dvec3(0, 10, 0);
    }
    
    // Buscar punto de respawn más cercano
    auto respawnPoints = getSpawnPointsByType(SpawnType::PLAYER_RESPAWN, it->second.zoneId);
    
    if (respawnPoints.empty()) {
        // Si no hay puntos de respawn, usar spawn inicial
        return getRandomSpawnPosition(SpawnType::PLAYER_START, it->second.zoneId);
    }
    
    // Encontrar el más cercano a donde murió
    SpawnPoint* closest = nullptr;
    double minDistance = std::numeric_limits<double>::max();
    
    for (auto& point : respawnPoints) {
        double distance = glm::length(point.position - it->second.deathPosition);
        if (distance < minDistance) {
            minDistance = distance;
            closest = getSpawnPoint(point.id);
        }
    }
    
    if (closest) {
        std::cout << "[SpawnSystem] Respawning player at: " << closest->id << std::endl;
        return getRandomPositionInRadius(closest->position, closest->radius);
    }
    
    return glm::dvec3(0, 10, 0);
}

void SpawnSystem::respawnPlayer(const std::string& userId) {
    auto it = pendingRespawns.find(userId);
    if (it != pendingRespawns.end() && it->second.canRespawn) {
        glm::dvec3 respawnPos = getRespawnPosition(userId);
        
        if (respawnCallback) {
            respawnCallback(userId);
        }
        
        pendingRespawns.erase(it);
        std::cout << "[SpawnSystem] Player respawned: " << userId << std::endl;
    }
}

void SpawnSystem::setPlayerCheckpoint(const std::string& userId, const std::string& checkpointId) {
    playerCheckpoints[userId] = checkpointId;
    std::cout << "[SpawnSystem] Checkpoint set for player " << userId << ": " << checkpointId << std::endl;
}

std::string SpawnSystem::getPlayerCheckpoint(const std::string& userId) {
    auto it = playerCheckpoints.find(userId);
    return (it != playerCheckpoints.end()) ? it->second : "";
}

void SpawnSystem::update(float deltaTime) {
    // Actualizar timers de respawn
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    
    for (auto& [userId, data] : pendingRespawns) {
        if (!data.canRespawn) {
            uint64_t elapsed = (now - data.deathTime) / 1000000; // ms
            if (elapsed >= data.respawnTimer * 1000) {
                data.canRespawn = true;
                std::cout << "[SpawnSystem] Player can respawn: " << userId << std::endl;
            }
        }
    }
}

glm::dvec3 SpawnSystem::getRandomPositionInRadius(const glm::dvec3& center, float radius) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(-radius, radius);
    
    glm::dvec3 offset(dist(gen), 0, dist(gen));
    return center + offset;
}

SpawnPoint* SpawnSystem::findBestSpawnPoint(SpawnType type, const std::string& zoneId) {
    SpawnPoint* best = nullptr;
    int minEntities = std::numeric_limits<int>::max();
    
    for (auto& [id, point] : spawnPoints) {
        if (point.type == type && point.active && 
            (zoneId.empty() || point.zoneId == zoneId)) {
            
            // Comprobar cooldown
            auto now = std::chrono::system_clock::now().time_since_epoch().count();
            if (point.lastSpawn > 0 && (now - point.lastSpawn) < point.cooldown * 1000000) {
                continue;
            }
            
            // Comprobar capacidad
            if (point.currentEntities >= point.maxEntities) {
                continue;
            }
            
            // Elegir el menos poblado
            if (point.currentEntities < minEntities) {
                minEntities = point.currentEntities;
                best = &point;
            }
        }
    }
    
    return best;
}

}