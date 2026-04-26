#include "database_manager.h"

#include <iostream>
#include <sstream>
#include <cstring>

namespace Haruka {

DatabaseConnection::DatabaseConnection(const std::string& connInfo)
    : connectionInfo(connInfo) {}

DatabaseConnection::~DatabaseConnection() {
    disconnect();
}

bool DatabaseConnection::connect() {
    conn = PQconnectdb(connectionInfo.c_str());
    
    if (PQstatus(conn) != CONNECTION_OK) {
        std::cerr << "[DB] Connection failed: " << PQerrorMessage(conn) << std::endl;
        PQfinish(conn);
        conn = nullptr;
        return false;
    }
    
    std::cout << "[DB] Connected successfully" << std::endl;
    return true;
}

void DatabaseConnection::disconnect() {
    if (conn) {
        PQfinish(conn);
        conn = nullptr;
    }
}

PGresult* DatabaseConnection::executeQuery(const std::string& query) {
    if (!conn) return nullptr;
    
    PGresult* res = PQexec(conn, query.c_str());
    
    if (PQresultStatus(res) != PGRES_TUPLES_OK && PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::cerr << "[DB] Query failed: " << PQerrorMessage(conn) << std::endl;
        PQclear(res);
        return nullptr;
    }
    
    return res;
}

bool DatabaseConnection::executeUpdate(const std::string& query) {
    PGresult* res = executeQuery(query);
    if (res) {
        PQclear(res);
        return true;
    }
    return false;
}

std::string DatabaseConnection::escapeString(const std::string& str) {
    if (!conn) return str;
    
    char* escaped = PQescapeLiteral(conn, str.c_str(), str.length());
    std::string result(escaped);
    PQfreemem(escaped);
    return result;
}

DatabaseManager::DatabaseManager() {}

void DatabaseManager::initDatabase(DatabaseType type, const std::string& host, 
                                   int port, const std::string& dbName,
                                   const std::string& user, const std::string& password) {
    std::stringstream connInfo;
    connInfo << "host=" << host 
             << " port=" << port 
             << " dbname=" << dbName 
             << " user=" << user 
             << " password=" << password;
    
    auto conn = std::make_unique<DatabaseConnection>(connInfo.str());
    
    if (conn->connect()) {
        connections[type] = std::move(conn);
        std::cout << "[DB] Initialized database type: " << (int)type << std::endl;
        
        // Create tables
        if (type == DatabaseType::WEB) {
            connections[type]->executeUpdate(
                "CREATE TABLE IF NOT EXISTS users ("
                "user_id VARCHAR(64) PRIMARY KEY, "
                "username VARCHAR(255) NOT NULL, "
                "email VARCHAR(255) UNIQUE NOT NULL, "
                "password_hash VARCHAR(255) NOT NULL, "
                "token VARCHAR(255), "
                "verified BOOLEAN DEFAULT FALSE, "
                "created_at BIGINT NOT NULL)"
            );
        }
        else if (type == DatabaseType::HEADMASTER) {
            connections[type]->executeUpdate(
                "CREATE TABLE IF NOT EXISTS player_data ("
                "user_id VARCHAR(64) PRIMARY KEY, "
                "character_id VARCHAR(64), "
                "pos_x DOUBLE PRECISION, "
                "pos_y DOUBLE PRECISION, "
                "pos_z DOUBLE PRECISION, "
                "inventory TEXT, "
                "level INTEGER DEFAULT 1, "
                "experience INTEGER DEFAULT 0, "
                "last_save BIGINT NOT NULL)"
            );
        }
        else if (type == DatabaseType::COMMS) {
            connections[type]->executeUpdate(
                "CREATE TABLE IF NOT EXISTS messages ("
                "message_id VARCHAR(64) PRIMARY KEY, "
                "sender_id VARCHAR(64), "
                "recipient_id VARCHAR(64), "
                "content TEXT, "
                "timestamp BIGINT NOT NULL, "
                "delivered BOOLEAN DEFAULT FALSE)"
            );
        }
        else if (type == DatabaseType::ANTICHEAT) {
            connections[type]->executeUpdate(
                "CREATE TABLE IF NOT EXISTS anticheat_logs ("
                "log_id VARCHAR(64) PRIMARY KEY, "
                "user_id VARCHAR(64), "
                "detection_type VARCHAR(255), "
                "data TEXT, "
                "timestamp BIGINT NOT NULL)"
            );
        }
    }
}

DatabaseConnection* DatabaseManager::getConnection(DatabaseType type) {
    auto it = connections.find(type);
    return (it != connections.end()) ? it->second.get() : nullptr;
}

// WEB DB
bool DatabaseManager::createUser(const UserData& user) {
    auto* conn = getConnection(DatabaseType::WEB);
    if (!conn) return false;
    
    std::stringstream query;
    query << "INSERT INTO users (user_id, username, email, password_hash, token, verified, created_at) "
          << "VALUES ("
          << conn->escapeString(user.userId) << ", "
          << conn->escapeString(user.username) << ", "
          << conn->escapeString(user.email) << ", "
          << conn->escapeString(user.passwordHash) << ", "
          << conn->escapeString(user.token) << ", "
          << (user.verified ? "TRUE" : "FALSE") << ", "
          << user.createdAt << ")";
    
    return conn->executeUpdate(query.str());
}

UserData* DatabaseManager::getUserByToken(const std::string& token) {
    auto* conn = getConnection(DatabaseType::WEB);
    if (!conn) return nullptr;
    
    std::string query = "SELECT * FROM users WHERE token=" + conn->escapeString(token);
    PGresult* res = conn->executeQuery(query);
    
    if (!res || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return nullptr;
    }
    
    UserData* user = new UserData();
    user->userId = PQgetvalue(res, 0, 0);
    user->username = PQgetvalue(res, 0, 1);
    user->email = PQgetvalue(res, 0, 2);
    user->passwordHash = PQgetvalue(res, 0, 3);
    user->token = PQgetvalue(res, 0, 4);
    user->verified = strcmp(PQgetvalue(res, 0, 5), "t") == 0;
    user->createdAt = std::stoull(PQgetvalue(res, 0, 6));
    
    PQclear(res);
    return user;
}

bool DatabaseManager::verifyUser(const std::string& userId, const std::string& token) {
    auto* user = getUserByToken(token);
    if (user && user->userId == userId) {
        delete user;
        return true;
    }
    delete user;
    return false;
}

// HEADMASTER DB
bool DatabaseManager::savePlayerData(const PlayerPersistence& data) {
    auto* conn = getConnection(DatabaseType::HEADMASTER);
    if (!conn) return false;
    
    std::stringstream query;
    query << "INSERT INTO player_data (user_id, character_id, pos_x, pos_y, pos_z, inventory, level, experience, last_save) "
          << "VALUES ("
          << conn->escapeString(data.userId) << ", "
          << conn->escapeString(data.characterId) << ", "
          << data.posX << ", " << data.posY << ", " << data.posZ << ", "
          << conn->escapeString(data.inventory) << ", "
          << data.level << ", " << data.experience << ", "
          << data.lastSave << ") "
          << "ON CONFLICT (user_id) DO UPDATE SET "
          << "pos_x=" << data.posX << ", pos_y=" << data.posY << ", pos_z=" << data.posZ << ", "
          << "inventory=" << conn->escapeString(data.inventory) << ", "
          << "level=" << data.level << ", experience=" << data.experience << ", "
          << "last_save=" << data.lastSave;
    
    return conn->executeUpdate(query.str());
}

PlayerPersistence* DatabaseManager::loadPlayerData(const std::string& userId) {
    auto* conn = getConnection(DatabaseType::HEADMASTER);
    if (!conn) return nullptr;
    
    std::string query = "SELECT * FROM player_data WHERE user_id=" + conn->escapeString(userId);
    PGresult* res = conn->executeQuery(query);
    
    if (!res || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return nullptr;
    }
    
    PlayerPersistence* data = new PlayerPersistence();
    data->userId = PQgetvalue(res, 0, 0);
    data->characterId = PQgetvalue(res, 0, 1);
    data->posX = std::stod(PQgetvalue(res, 0, 2));
    data->posY = std::stod(PQgetvalue(res, 0, 3));
    data->posZ = std::stod(PQgetvalue(res, 0, 4));
    data->inventory = PQgetvalue(res, 0, 5);
    data->level = std::stoi(PQgetvalue(res, 0, 6));
    data->experience = std::stoi(PQgetvalue(res, 0, 7));
    data->lastSave = std::stoull(PQgetvalue(res, 0, 8));
    
    PQclear(res);
    return data;
}

bool DatabaseManager::updatePlayerPosition(const std::string& userId, double x, double y, double z) {
    auto* conn = getConnection(DatabaseType::HEADMASTER);
    if (!conn) return false;
    
    std::stringstream query;
    query << "UPDATE player_data SET pos_x=" << x << ", pos_y=" << y << ", pos_z=" << z
          << " WHERE user_id=" << conn->escapeString(userId);
    
    return conn->executeUpdate(query.str());
}

// COMMS DB
bool DatabaseManager::saveMessage(const MessageLog& msg) {
    auto* conn = getConnection(DatabaseType::COMMS);
    if (!conn) return false;
    
    std::stringstream query;
    query << "INSERT INTO messages (message_id, sender_id, recipient_id, content, timestamp, delivered) "
          << "VALUES ("
          << conn->escapeString(msg.messageId) << ", "
          << conn->escapeString(msg.senderId) << ", "
          << conn->escapeString(msg.recipientId) << ", "
          << conn->escapeString(msg.content) << ", "
          << msg.timestamp << ", "
          << (msg.delivered ? "TRUE" : "FALSE") << ")";
    
    return conn->executeUpdate(query.str());
}

std::vector<MessageLog> DatabaseManager::getUndeliveredMessages(const std::string& recipientId) {
    std::vector<MessageLog> messages;
    auto* conn = getConnection(DatabaseType::COMMS);
    if (!conn) return messages;
    
    std::string query = "SELECT * FROM messages WHERE recipient_id=" + 
                       conn->escapeString(recipientId) + " AND delivered=FALSE";
    PGresult* res = conn->executeQuery(query);
    
    if (!res) return messages;
    
    int rows = PQntuples(res);
    for (int i = 0; i < rows; i++) {
        MessageLog msg;
        msg.messageId = PQgetvalue(res, i, 0);
        msg.senderId = PQgetvalue(res, i, 1);
        msg.recipientId = PQgetvalue(res, i, 2);
        msg.content = PQgetvalue(res, i, 3);
        msg.timestamp = std::stoull(PQgetvalue(res, i, 4));
        msg.delivered = strcmp(PQgetvalue(res, i, 5), "t") == 0;
        messages.push_back(msg);
    }
    
    PQclear(res);
    return messages;
}

bool DatabaseManager::markMessageDelivered(const std::string& messageId) {
    auto* conn = getConnection(DatabaseType::COMMS);
    if (!conn) return false;
    
    std::string query = "UPDATE messages SET delivered=TRUE WHERE message_id=" + 
                       conn->escapeString(messageId);
    
    return conn->executeUpdate(query);
}

// ANTICHEAT DB
bool DatabaseManager::logDetection(const AnticheatLog& log) {
    auto* conn = getConnection(DatabaseType::ANTICHEAT);
    if (!conn) return false;
    
    std::stringstream query;
    query << "INSERT INTO anticheat_logs (log_id, user_id, detection_type, data, timestamp) "
          << "VALUES ("
          << conn->escapeString(log.logId) << ", "
          << conn->escapeString(log.userId) << ", "
          << conn->escapeString(log.detectionType) << ", "
          << conn->escapeString(log.data) << ", "
          << log.timestamp << ")";
    
    return conn->executeUpdate(query.str());
}

std::vector<AnticheatLog> DatabaseManager::getUserLogs(const std::string& userId) {
    std::vector<AnticheatLog> logs;
    auto* conn = getConnection(DatabaseType::ANTICHEAT);
    if (!conn) return logs;
    
    std::string query = "SELECT * FROM anticheat_logs WHERE user_id=" + conn->escapeString(userId);
    PGresult* res = conn->executeQuery(query);
    
    if (!res) return logs;
    
    int rows = PQntuples(res);
    for (int i = 0; i < rows; i++) {
        AnticheatLog log;
        log.logId = PQgetvalue(res, i, 0);
        log.userId = PQgetvalue(res, i, 1);
        log.detectionType = PQgetvalue(res, i, 2);
        log.data = PQgetvalue(res, i, 3);
        log.timestamp = std::stoull(PQgetvalue(res, i, 4));
        logs.push_back(log);
    }
    
    PQclear(res);
    return logs;
}

}