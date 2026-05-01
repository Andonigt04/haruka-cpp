/**
 * @file ingame_chat.h
 * @brief In-game chat UI and transport bridge (local display or UDP/network mode).
 */
#pragma once

#include "chat_system.h"
#include "network/network_manager.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <thread>
#include <atomic>

namespace Haruka {

/**
 * @brief In-game chat UI and transport bridge.
 *
 * Supports local display, optional network transport, and buffered history.
 */
class InGameChat {
public:
    /** @brief Constructs a chat instance for one player. */
    InGameChat(const std::string& playerName, int localPort, int remotePort);
    /** @brief Releases chat resources. */
    ~InGameChat();
    
    /** @brief Initializes sockets or callbacks. */
    void init();
    /** @brief Stops background receive thread and closes sockets. */
    void shutdown();
    
    /** @brief Advances UI/transport state. */
    void update(float deltaTime);
    /** @brief Renders chat UI. */
    void render();
    
    /** @brief Sends a message through the selected transport. */
    void sendMessage(const std::string& message);

    /** @brief Sets the network client and enables network mode when non-null. */
    void setNetworkClient(NetworkClient* client) { networkClient = client; useNetwork = (client != nullptr); }
    /** @brief Enables or disables network transport. */
    void setUseNetwork(bool enabled) { useNetwork = enabled; }

    /** @brief Returns true when the chat window is open. */
    bool isOpen() const { return chatOpen; }
    /** @brief Toggles the chat window open/closed. */
    void toggleChat() { chatOpen = !chatOpen; }
    /** @brief Opens the chat window. */
    void openChat() { chatOpen = true; }
    /** @brief Closes the chat window. */
    void closeChat() { chatOpen = false; }

private:
    std::string playerName;
    int localPort;
    int remotePort;

    NetworkClient* networkClient = nullptr;
    bool useNetwork = false;
    
    int sockfd = -1;
    sockaddr_in remoteAddr;
    std::atomic<bool> running{false};
    std::thread receiveThread;
    
    bool chatOpen = false;
    char inputBuffer[256] = {0};
    
    std::vector<std::string> messageHistory;
    int maxMessages = 50;
    
    /** @brief Background receive loop for UDP transport. */
    void receiveMessages();
    /** @brief Appends a message to local history. */
    void addMessage(const std::string& message);
};

}