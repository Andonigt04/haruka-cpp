/**
 * @file database_manager.h
 * @brief PostgreSQL persistence layer — per-role connections (web, headmaster, comms, anticheat).
 */
#pragma once

#include <string>
#include <memory>
#include <vector>
#include <map>
#include <libpq-fe.h>

namespace Haruka {

/** @brief Database roles supported by the persistence layer. */
enum class DatabaseType {
    WEB,           // Usuarios web, tokens, sesiones
    HEADMASTER,    // Persistencia jugadores, inventarios, mundo
    COMMS,         // Mensajes, chat, logs
    ANTICHEAT      // Logs de detecciones, reportes
};

/** @brief Web-facing user account row. */
struct UserData {
    std::string userId;
    std::string username;
    std::string email;
    std::string passwordHash;
    std::string token;
    bool verified;
    uint64_t createdAt;
};

/** @brief Persisted player state for headmaster storage. */
struct PlayerPersistence {
    std::string userId;
    std::string characterId;
    double posX, posY, posZ;
    std::string inventory;
    int level;
    int experience;
    uint64_t lastSave;
};

/** @brief Message delivery log record. */
struct MessageLog {
    std::string messageId;
    std::string senderId;
    std::string recipientId;
    std::string content;
    uint64_t timestamp;
    bool delivered;
};

/** @brief Anti-cheat detection log record. */
struct AnticheatLog {
    std::string logId;
    std::string userId;
    std::string detectionType;
    std::string data;
    uint64_t timestamp;
};

/**
 * @brief Thin PostgreSQL connection wrapper.
 *
 * Owns one `PGconn` handle and exposes query/update helpers.
 */
class DatabaseConnection {
public:
    /** @brief Opens a connection using provided libpq connection string. */
    DatabaseConnection(const std::string& connInfo);
    ~DatabaseConnection();
    
    /** @brief Returns true if the connection handle is valid. */
    bool connect();
    /** @brief Closes the active connection if present. */
    void disconnect();
    /** @brief Returns true if the underlying connection exists. */
    bool isConnected() const { return conn != nullptr; }
    
    /** @brief Executes a SQL query and returns the raw result handle. */
    PGresult* executeQuery(const std::string& query);
    /** @brief Executes a SQL update statement. */
    bool executeUpdate(const std::string& query);
    
    /** @brief Escapes a string for safe SQL embedding. */
    std::string escapeString(const std::string& str);

private:
    PGconn* conn = nullptr;
    std::string connectionInfo;
};

/**
 * @brief Project-wide database manager singleton.
 *
 * Owns per-role connections and exposes persistence helpers for gameplay,
 * networking and moderation subsystems.
 */
class DatabaseManager {
public:
    static DatabaseManager& getInstance() {
        static DatabaseManager instance;
        return instance;
    }
    
    /** @brief Initializes a database connection for the requested role. */
    void initDatabase(DatabaseType type, const std::string& host, 
                     int port, const std::string& dbName,
                     const std::string& user, const std::string& password);
    
    /** @brief Returns the active connection for a role, if any. */
    DatabaseConnection* getConnection(DatabaseType type);
    
    /** @name Web database operations */
    ///@{
    bool createUser(const UserData& user);
    UserData* getUserByToken(const std::string& token);
    bool verifyUser(const std::string& userId, const std::string& token);
    ///@}
    
    /** @name Headmaster persistence operations */
    ///@{
    bool savePlayerData(const PlayerPersistence& data);
    PlayerPersistence* loadPlayerData(const std::string& userId);
    bool updatePlayerPosition(const std::string& userId, double x, double y, double z);
    ///@}
    
    /** @name Communications log operations */
    ///@{
    bool saveMessage(const MessageLog& msg);
    std::vector<MessageLog> getUndeliveredMessages(const std::string& recipientId);
    bool markMessageDelivered(const std::string& messageId);
    ///@}
    
    /** @name Anti-cheat log operations */
    ///@{
    bool logDetection(const AnticheatLog& log);
    std::vector<AnticheatLog> getUserLogs(const std::string& userId);
    ///@}

private:
    /** @brief Builds singleton without external ownership. */
    DatabaseManager();
    std::map<DatabaseType, std::unique_ptr<DatabaseConnection>> connections;
};

}