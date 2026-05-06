#include "network_manager.h"
#include <iostream>
#include <sstream>
#include <chrono>

namespace Haruka {

// ============ NetworkClient ============
NetworkClient::NetworkClient() {}
NetworkClient::~NetworkClient() { disconnect(); }

bool NetworkClient::connect(const std::string& serverAddr, int port) {
    std::cout << "[Client] Connecting to Comms: " << serverAddr << ":" << port << std::endl;
    connected = true;
    serverId = serverAddr + ":" + std::to_string(port);
    return true;
}

void NetworkClient::disconnect() {
    connected = false;
    std::cout << "[Client] Disconnected" << std::endl;
}

void NetworkClient::sendPlayerData(const PlayerData& data) {
    if (!connected) return;
    
    NetworkMessage msg;
    msg.messageType = "PLAYER_LOGIN";
    msg.senderId = data.userId;
    msg.recipientId = "COMMS";
    msg.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    
    std::cout << "[Client] Sending login" << std::endl;
}

void NetworkClient::sendPositionUpdate(Haruka::WorldPos pos, Haruka::Rotation rot) {
    if (!connected) return;
    
    NetworkMessage msg;
    msg.messageType = "POSITION_UPDATE";
    msg.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
}

void NetworkClient::sendChatMessage(const std::string& senderName, const std::string& content) {
    if (!connected) return;

    NetworkMessage msg;
    msg.messageType = "CHAT";
    msg.senderId = senderName;
    msg.recipientId = "COMMS";
    msg.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    msg.payload = content;

    std::cout << "[Client] Sending CHAT: " << content << std::endl;
}

void NetworkClient::handleMessage(const NetworkMessage& msg) {
    std::cout << "[Client] Received: " << msg.messageType << std::endl;
    if (messageCallback) messageCallback(msg);
}

// ============ NetworkServer ============
NetworkServer::NetworkServer(ServerType type) : serverType(type) {}
NetworkServer::~NetworkServer() { stop(); }

bool NetworkServer::start(int port) {
    running = true;
    std::cout << "[Server] Started on port " << port << " (type: " << (int)serverType << ")" << std::endl;
    return true;
}

void NetworkServer::stop() {
    running = false;
    std::cout << "[Server] Stopped" << std::endl;
}

void NetworkServer::registerServer(const ServerInfo& info) {
    connectedServers[info.serverId] = info;
    std::cout << "[Server] Registered: " << info.serverId << std::endl;
}

void NetworkServer::broadcastMessage(const NetworkMessage& msg) {
    std::cout << "[Server] Broadcasting: " << msg.messageType << std::endl;
}

void NetworkServer::handleChatMessage(const NetworkMessage& msg) {
    std::cout << "[COMMS] Chat: " << msg.senderId << ": " << msg.payload << std::endl;
    broadcastChat(msg);
}

void NetworkServer::broadcastChat(const NetworkMessage& msg) {
    // TODO: enviar a clientes conectados
    std::cout << "[COMMS] Broadcast chat" << std::endl;
}

void NetworkServer::routeMessage(const NetworkMessage& msg) {
    if (serverType == ServerType::COMMS) {
        if (msg.messageType == "CHAT") {
            handleChatMessage(msg);
            return;
        }
        relayToHeadmaster(msg);
    }
    else if (serverType == ServerType::HEADMASTER) {
        std::cout << "[HEADMASTER] Processing: " << msg.messageType << std::endl;
    }
    else if (serverType == ServerType::ZONE) {
        std::cout << "[ZONE] Processing in zone" << std::endl;
    }
}

// HEADMASTER
void NetworkServer::loadPlayerPersistence(const std::string& userId) {
    std::cout << "[HEADMASTER] Loading persistence for " << userId << std::endl;
}

void NetworkServer::savePlayerPersistence(const PlayerData& data) {
    std::cout << "[HEADMASTER] Saving persistence for " << data.userId << std::endl;
}

void NetworkServer::syncAllServers() {
    std::cout << "[HEADMASTER] Syncing all servers" << std::endl;
}

// CLUSTER_MANAGER
void NetworkServer::registerZone(const ZoneInfo& zone) {
    zones[zone.zoneId] = zone;
    std::cout << "[CLUSTER] Registered zone: " << zone.zoneId << std::endl;
}

std::string NetworkServer::assignPlayerToZone(const std::string& userId, Haruka::WorldPos position) {
    std::cout << "[CLUSTER] Assigning player " << userId << " to zone" << std::endl;
    return "zone_0";
}

void NetworkServer::balanceZones() {
    std::cout << "[CLUSTER] Balancing zones" << std::endl;
}

std::vector<ZoneInfo> NetworkServer::getActiveZones() {
    std::vector<ZoneInfo> activeZones;
    for (auto& [id, zone] : zones) {
        if (zone.active) activeZones.push_back(zone);
    }
    return activeZones;
}

// WEB_AUTH
bool NetworkServer::authenticateUser(const std::string& userId, const std::string& token) {
    std::cout << "[WEB_AUTH] Authenticating user: " << userId << std::endl;
    return true;
}

PlayerData NetworkServer::loadUserProfile(const std::string& userId) {
    std::cout << "[WEB_AUTH] Loading profile for " << userId << std::endl;
    return PlayerData();
}

// ANTICHEAT
void NetworkServer::checkPlayerBehavior(const std::string& userId, const PlayerData& data) {
    std::cout << "[ANTICHEAT] Checking player: " << userId << std::endl;
}

bool NetworkServer::validateMovement(Haruka::WorldPos oldPos, Haruka::WorldPos newPos, float deltaTime) {
    double distance = glm::length(newPos - oldPos);
    double maxSpeed = 50.0;
    return distance / deltaTime < maxSpeed;
}

// COMMS
void NetworkServer::relayToHeadmaster(const NetworkMessage& msg) {
    std::cout << "[COMMS] Relaying to Headmaster" << std::endl;
}

void NetworkServer::relayToClient(const NetworkMessage& msg) {
    std::cout << "[COMMS] Relaying to Client" << std::endl;
}

// ZONE
void NetworkServer::updateZonePlayers() {
    std::cout << "[ZONE] Updating players" << std::endl;
}

void NetworkServer::sendToCluster(const NetworkMessage& msg) {
    std::cout << "[ZONE] Sending to Cluster" << std::endl;
}

// ============ NetworkManager ============
NetworkManager::NetworkManager() {
    client = std::make_unique<NetworkClient>();
}

void NetworkManager::initClient(const std::string& addr, int port) {
    client->connect(addr, port);
}

void NetworkManager::initServer(ServerType type, int port) {
    server = std::make_unique<NetworkServer>(type);
    server->start(port);
}

void NetworkManager::update(double deltaTime) {
    timeSinceSync += deltaTime;
    if (timeSinceSync >= syncInterval) {
        timeSinceSync = 0.0;
    }
}

}