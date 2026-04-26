#include "websocket_client.h"

#include <iostream>

namespace Haruka {

WebSocketClient::WebSocketClient() {
    client.set_access_channels(websocketpp::log::alevel::none);
    client.set_error_channels(websocketpp::log::elevel::none);
    
    client.init_asio();
    
    client.set_open_handler(bind(&WebSocketClient::onOpenHandler, this, std::placeholders::_1));
    client.set_message_handler(bind(&WebSocketClient::onMessageHandler, this, std::placeholders::_1, std::placeholders::_2));
    client.set_close_handler(bind(&WebSocketClient::onCloseHandler, this, std::placeholders::_1));
}

WebSocketClient::~WebSocketClient() {
    disconnect();
}

bool WebSocketClient::connect(const std::string& uri) {
    try {
        websocketpp::lib::error_code ec;
        WsClient::connection_ptr con = client.get_connection(uri, ec);
        
        if (ec) {
            std::cerr << "[WS] Connection error: " << ec.message() << std::endl;
            return false;
        }
        
        handle = con->get_handle();
        client.connect(con);
        
        // Correr cliente en thread
        std::thread([this]() {
            client.run();
        }).detach();
        
        std::cout << "[WS] Connecting to " << uri << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[WS] Exception: " << e.what() << std::endl;
        return false;
    }
}

void WebSocketClient::disconnect() {
    if (connected) {
        client.close(handle, websocketpp::close::status::normal, "");
        connected = false;
    }
}

void WebSocketClient::send(const std::string& message) {
    if (connected) {
        client.send(handle, message, websocketpp::frame::opcode::text);
    }
}

void WebSocketClient::onOpenHandler(WsHandle hdl) {
    connected = true;
    std::cout << "[WS] Connected" << std::endl;
    if (onOpen) onOpen();
}

void WebSocketClient::onMessageHandler(WsHandle hdl, WsClient::message_ptr msg) {
    std::cout << "[WS] Message: " << msg->get_payload() << std::endl;
    if (onMessage) onMessage(msg->get_payload());
}

void WebSocketClient::onCloseHandler(WsHandle hdl) {
    connected = false;
    std::cout << "[WS] Disconnected" << std::endl;
    if (onClose) onClose();
}

}