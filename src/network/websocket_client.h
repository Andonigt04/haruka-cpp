/**
 * @file websocket_client.h
 * @brief websocketpp asynchronous WebSocket client wrapper.
 */
#pragma once

#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <thread>
#include <functional>

namespace Haruka {

using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

typedef websocketpp::client<websocketpp::config::asio_client> WsClient;
typedef websocketpp::connection_hdl WsHandle;

/**
 * @brief Client wrapper around websocketpp asynchronous websocket transport.
 */
class WebSocketClient {
public:
    /** @brief Constructs a disconnected websocket client. */
    WebSocketClient();
    /** @brief Releases websocket client resources. */
    ~WebSocketClient();
    
    /** @brief Connects to the remote websocket endpoint. */
    bool connect(const std::string& uri);
    /** @brief Disconnects from the current endpoint. */
    void disconnect();
    /** @brief Returns true while connected. */
    bool isConnected() const { return connected; }
    
    /** @brief Sends one websocket text message. */
    void send(const std::string& message);
    
    /** @brief Registers open callback. */
    void setOpenCallback(std::function<void()> cb) { onOpen = cb; }
    /** @brief Registers message callback. */
    void setMessageCallback(std::function<void(const std::string&)> cb) { onMessage = cb; }
    /** @brief Registers close callback. */
    void setCloseCallback(std::function<void()> cb) { onClose = cb; }

private:
    WsClient client;
    WsHandle handle;
    bool connected = false;
    
    std::function<void()> onOpen;
    std::function<void(const std::string&)> onMessage;
    std::function<void()> onClose;
    
    /** @brief Internal open event handler. */
    void onOpenHandler(WsHandle hdl);
    /** @brief Internal message event handler. */
    void onMessageHandler(WsHandle hdl, WsClient::message_ptr msg);
    /** @brief Internal close event handler. */
    void onCloseHandler(WsHandle hdl);
};

}