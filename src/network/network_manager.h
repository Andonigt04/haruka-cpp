#pragma once

#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <map>

namespace Haruka {

/** @brief Enumeration of supported server roles in the network stack. */
enum class ServerType {
    HEADMASTER,      // Persistence + central coordination
    CLUSTER_MANAGER, // Zone organization and distribution
    WEB_AUTH,        // Loads web user profiles
    ANTICHEAT,       // Behavior verification
    COMMS,           // Client <-> headmaster communications
    ZONE             // Individual zone server
};

/** @brief Persisted player profile and live session metadata. */
struct PlayerData {
    std::string userId;
    std::string username;
    std::string email;
    glm::dvec3 position;
    glm::vec3 rotation;
    std::string currentZone;
    uint64_t lastUpdate;
    bool verified;
    bool authenticated;
};

/** @brief Zone allocation and load balancing metadata. */
struct ZoneInfo {
    std::string zoneId;
    std::string address;
    int port;
    int playerCount;
    int maxPlayers;
    float load; // 0.0 - 1.0
    bool active;
    glm::dvec3 worldPosition;
    double radius;
};

/** @brief Describes a registered server instance. */
struct ServerInfo {
    std::string serverId;
    ServerType type;
    std::string address;
    int port;
    bool active;
};

/** @brief Generic network payload envelope. */
struct NetworkMessage {
    std::string messageType;
    std::string senderId;
    std::string recipientId;
    std::string payload;
    uint64_t timestamp;
};

/**
 * @brief Client-side network façade for player and chat communication.
 */
class NetworkClient {
public:
    /** @brief Constructs a disconnected client. */
    NetworkClient();
    ~NetworkClient();
    
    /** @brief Opens a client connection to a server endpoint. */
    bool connect(const std::string& serverAddr, int port);
    /** @brief Closes the current connection. */
    void disconnect();
    /** @brief Returns true when client is connected. */
    bool isConnected() const { return connected; }
    
    /** @brief Sends player state payload to server. */
    void sendPlayerData(const PlayerData& data);
    /** @brief Sends position/rotation update payload. */
    void sendPositionUpdate(glm::dvec3 pos, glm::vec3 rot);
    /** @brief Handles one incoming message. */
    void handleMessage(const NetworkMessage& msg);
    /** @brief Sends a chat message payload. */
    void sendChatMessage(const std::string& senderName, const std::string& content);
    
    void setMessageCallback(std::function<void(const NetworkMessage&)> cb) {
        messageCallback = cb;
    }

private:
    bool connected = false;
    std::string serverId;
    std::function<void(const NetworkMessage&)> messageCallback;
};

/**
 * @brief Server-side network façade for headmaster/cluster/zone roles.
 */
class NetworkServer {
public:
    /** @brief Creates server instance for the requested role. */
    NetworkServer(ServerType type);
    ~NetworkServer();
    
    /** @brief Starts listening on the given port. */
    bool start(int port);
    /** @brief Stops server loops and disconnects clients. */
    void stop();
    /** @brief Returns true while server is active. */
    bool isRunning() const { return running; }
    
    void registerServer(const ServerInfo& info);
    void broadcastMessage(const NetworkMessage& msg);
    void routeMessage(const NetworkMessage& msg);
    void handleChatMessage(const NetworkMessage& msg);
    void broadcastChat(const NetworkMessage& msg);
    
    /** @name Headmaster responsibilities */
    ///@{
    void loadPlayerPersistence(const std::string& userId);
    void savePlayerPersistence(const PlayerData& data);
    void syncAllServers();
    ///@}
    
    /** @name Cluster manager responsibilities */
    ///@{
    void registerZone(const ZoneInfo& zone);
    std::string assignPlayerToZone(const std::string& userId, glm::dvec3 position);
    void balanceZones();
    std::vector<ZoneInfo> getActiveZones();
    ///@}
    
    /** @name Web authentication responsibilities */
    ///@{
    bool authenticateUser(const std::string& userId, const std::string& token);
    PlayerData loadUserProfile(const std::string& userId);
    ///@}
    
    /** @name Anti-cheat responsibilities */
    ///@{
    void checkPlayerBehavior(const std::string& userId, const PlayerData& data);
    bool validateMovement(glm::dvec3 oldPos, glm::dvec3 newPos, float deltaTime);
    ///@}
    
    /** @name Relay responsibilities */
    ///@{
    void relayToHeadmaster(const NetworkMessage& msg);
    void relayToClient(const NetworkMessage& msg);
    ///@}
    
    /** @name Zone-server responsibilities */
    ///@{
    void updateZonePlayers();
    void sendToCluster(const NetworkMessage& msg);
    ///@}

private:
    ServerType serverType;
    bool running = false;
    std::map<std::string, ServerInfo> connectedServers;
    std::map<std::string, PlayerData> players;
    std::map<std::string, ZoneInfo> zones;
    
    std::string headmasterAddress = "localhost";
    int headmasterPort = 8080;
    std::string clusterAddress = "localhost";
    int clusterPort = 8083;
};

/**
 * @brief Singleton that owns client/server networking entry points.
 */
class NetworkManager {
public:
    static NetworkManager& getInstance() {
        static NetworkManager instance;
        return instance;
    }
    
    /** @brief Returns managed client instance. */
    NetworkClient* getClient() { return client.get(); }
    /** @brief Initializes client networking connection. */
    void initClient(const std::string& addr, int port);
    
    /** @brief Returns managed server instance. */
    NetworkServer* getServer() { return server.get(); }
    /** @brief Initializes server side networking role. */
    void initServer(ServerType type, int port);
    
    /** @brief Advances sync timers and periodic network jobs. */
    void update(double deltaTime);

private:
    NetworkManager();
    std::unique_ptr<NetworkClient> client;
    std::unique_ptr<NetworkServer> server;
    double syncInterval = 0.1;
    double timeSinceSync = 0.0;
};

}