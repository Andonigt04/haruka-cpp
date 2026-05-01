/**
 * @file chat_system.h
 * @brief Chat state, command routing, mute/block, and multi-channel message history.
 */
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <deque>
#include <map>
#include <cstdint>

namespace Haruka {

/** @brief Supported chat routing channels. */
enum class ChatChannel {
    GLOBAL,
    LOCAL,
    PARTY,
    GUILD,
    WHISPER,
    SYSTEM
};

/** @brief Serialized chat message record. */
struct ChatMessage {
    std::string messageId;
    std::string senderId;
    std::string senderName;
    std::string recipientId;
    std::string content;
    ChatChannel channel;
    uint64_t timestamp;
    bool isCommand;
};

/**
 * @brief Chat state, command routing, and message history manager.
 */
class ChatSystem {
public:
    /** @brief Constructs an empty chat system. */
    ChatSystem();
    /** @brief Releases chat resources. */
    ~ChatSystem();
    
    /** @brief Sends a chat message to a channel. */
    void sendMessage(const std::string& content, ChatChannel channel = ChatChannel::GLOBAL);
    /** @brief Sends a private whisper. */
    void sendWhisper(const std::string& recipientId, const std::string& content);
    /** @brief Executes a chat command locally. */
    void sendCommand(const std::string& command);
    
    /** @brief Accepts an incoming message from the network. */
    void receiveMessage(const ChatMessage& msg);
    
    /** @brief Returns up to `maxMessages` from a channel history. */
    std::vector<ChatMessage> getHistory(ChatChannel channel = ChatChannel::GLOBAL, int maxMessages = 50);
    /** @brief Returns whisper history for a user. */
    std::vector<ChatMessage> getWhisperHistory(const std::string& userId, int maxMessages = 50);
    /** @brief Clears stored messages for a channel. */
    void clearHistory(ChatChannel channel);
    
    /** @brief Adds a user to the mute list. */
    void muteUser(const std::string& userId);
    /** @brief Removes a user from the mute list. */
    void unmuteUser(const std::string& userId);
    /** @brief Returns true if the user is muted. */
    bool isUserMuted(const std::string& userId);
    
    /** @brief Blocks a chat channel locally. */
    void blockChannel(ChatChannel channel);
    /** @brief Unblocks a chat channel locally. */
    void unblockChannel(ChatChannel channel);
    /** @brief Returns true if a channel is blocked. */
    bool isChannelBlocked(ChatChannel channel);
    
    /** @brief Registers a slash-command callback. */
    void registerCommand(const std::string& command, std::function<void(const std::vector<std::string>&)> callback);
    /** @brief Parses and executes a command line. */
    void executeCommand(const std::string& input);
    
    /** @brief Sets message-received callback. */
    void setMessageCallback(std::function<void(const ChatMessage&)> cb) { messageCallback = cb; }
    /** @brief Sets message-send callback. */
    void setSendCallback(std::function<void(const ChatMessage&)> cb) { sendCallback = cb; }

private:
    std::deque<ChatMessage> globalHistory;
    std::deque<ChatMessage> localHistory;
    std::deque<ChatMessage> partyHistory;
    std::deque<ChatMessage> guildHistory;
    std::deque<ChatMessage> systemHistory;
    std::map<std::string, std::deque<ChatMessage>> whisperHistory;
    
    std::vector<std::string> mutedUsers;
    std::vector<ChatChannel> blockedChannels;
    
    std::map<std::string, std::function<void(const std::vector<std::string>&)>> commands;
    
    std::function<void(const ChatMessage&)> messageCallback;
    std::function<void(const ChatMessage&)> sendCallback;
    
    int maxHistorySize = 100;
    
    /** @brief Appends a message to the appropriate history deque. */
    void addToHistory(const ChatMessage& msg);
    /** @brief Builds a chat message record from content/channel inputs. */
    ChatMessage createMessage(const std::string& content, ChatChannel channel);
    /** @brief Splits a command line into tokens. */
    std::vector<std::string> parseCommand(const std::string& input);
};

}