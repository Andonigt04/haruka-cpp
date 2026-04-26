#include "ingame_chat.h"

#include <imgui.h>
#include <iostream>
#include <cstring>
#include <arpa/inet.h>
#include <unistd.h>

namespace Haruka {

InGameChat::InGameChat(const std::string& playerName, int localPort, int remotePort)
    : playerName(playerName), localPort(localPort), remotePort(remotePort) {}

InGameChat::~InGameChat() {
    shutdown();
}

void InGameChat::init() {
    // Si usamos Comms server, no abrir UDP
    if (useNetwork && networkClient) {
        std::cout << "[Chat] Using network (Comms), UDP disabled" << std::endl;
        addMessage("=== Chat initialized (Comms) ===");
        return;
    }

    // UDP fallback
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        std::cerr << "[Chat] Error creating socket" << std::endl;
        return;
    }

    sockaddr_in localAddr;
    memset(&localAddr, 0, sizeof(localAddr));
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port = htons(localPort);

    if (bind(sockfd, (sockaddr*)&localAddr, sizeof(localAddr)) < 0) {
        std::cerr << "[Chat] Error binding socket" << std::endl;
        close(sockfd);
        sockfd = -1;
        return;
    }

    memset(&remoteAddr, 0, sizeof(remoteAddr));
    remoteAddr.sin_family = AF_INET;
    remoteAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    remoteAddr.sin_port = htons(remotePort);

    running = true;
    receiveThread = std::thread(&InGameChat::receiveMessages, this);

    std::cout << "[Chat] Initialized (local: " << localPort << ", remote: " << remotePort << ")" << std::endl;
    addMessage("=== Chat initialized (UDP) ===");
}

void InGameChat::shutdown() {
    running = false;
    if (receiveThread.joinable()) {
        receiveThread.join();
    }
    if (sockfd >= 0) {
        close(sockfd);
        sockfd = -1;
    }
}

void InGameChat::update(float deltaTime) {
    // Nada que actualizar, el thread recibe mensajes
}

void InGameChat::render() {
    if (!chatOpen) return;
    
    ImGui::SetNextWindowSize(ImVec2(500, 300), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(10, ImGui::GetIO().DisplaySize.y - 310), ImGuiCond_FirstUseEver);
    
    if (ImGui::Begin("Chat (Press T to close)", &chatOpen)) {
        // Message history
        ImGui::BeginChild("ChatHistory", ImVec2(0, -30), true);
        for (const auto& msg : messageHistory) {
            ImGui::TextWrapped("%s", msg.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
        
        // Input
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##ChatInput", inputBuffer, sizeof(inputBuffer), 
                            ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (strlen(inputBuffer) > 0) {
                sendMessage(inputBuffer);
                memset(inputBuffer, 0, sizeof(inputBuffer));
            }
            ImGui::SetKeyboardFocusHere(-1);
        }
        
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere(-1);
        }
    }
    ImGui::End();
}

void InGameChat::sendMessage(const std::string& message) {
    if (message.empty()) return;

    // Prefer Comms server
    if (useNetwork && networkClient) {
        networkClient->sendChatMessage(playerName, message);
        addMessage("[You] " + message);
        return;
    }

    // Fallback UDP (local testing)
    if (sockfd < 0) return;
    
    std::string fullMessage = playerName + ": " + message;
    sendto(sockfd, fullMessage.c_str(), fullMessage.length(), 0,
          (sockaddr*)&remoteAddr, sizeof(remoteAddr));
    
    addMessage("[You] " + message);
}

void InGameChat::receiveMessages() {
    char buffer[1024];
    sockaddr_in senderAddr;
    socklen_t senderLen = sizeof(senderAddr);
    
    while (running) {
        int n = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0,
                       (sockaddr*)&senderAddr, &senderLen);
        
        if (n > 0) {
            buffer[n] = '\0';
            addMessage(std::string(buffer));
            std::cout << "[Chat] Received: " << buffer << std::endl;
        }
    }
}

void InGameChat::addMessage(const std::string& message) {
    messageHistory.push_back(message);
    if (messageHistory.size() > maxMessages) {
        messageHistory.erase(messageHistory.begin());
    }
}

}