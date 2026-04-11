#include "socket_server.h"
#include <iostream>

namespace Haruka {

SocketServer::SocketServer(boost::asio::io_context& io_context, int port)
    : io_context(io_context) {
    
    tcp::endpoint endpoint(tcp::v4(), port);
    acceptor = std::make_unique<tcp::acceptor>(io_context, endpoint);
    std::cout << "[SocketServer] Listening on port " << port << std::endl;
}

SocketServer::~SocketServer() {
    stop();
}

void SocketServer::start() {
    running = true;
    asyncAccept();
    
    std::thread([this]() {
        io_context.run();
    }).detach();
    
    std::cout << "[SocketServer] Started" << std::endl;
}

void SocketServer::stop() {
    running = false;
    if (acceptor) {
        acceptor->close();
    }
    std::cout << "[SocketServer] Stopped" << std::endl;
}

void SocketServer::asyncAccept() {
    auto session = std::make_shared<SocketSession>(io_context);
    
    acceptor->async_accept(session->socket,
        [this, session](const boost::system::error_code& error) {
            handleAccept(session, error);
        });
}

void SocketServer::handleAccept(std::shared_ptr<SocketSession> session,
                                 const boost::system::error_code& error) {
    if (!error) {
        session->peerId = session->socket.remote_endpoint().address().to_string();
        sessions[session->peerId] = session;
        
        std::cout << "[SocketServer] Client connected: " << session->peerId << std::endl;
    }
    
    if (running) {
        asyncAccept();
    }
}

void SocketServer::broadcastMessage(const std::string& message) {
    for (auto& [peerId, session] : sessions) {
        sendMessage(peerId, message);
    }
}

void SocketServer::sendMessage(const std::string& peerId, const std::string& message) {
    auto it = sessions.find(peerId);
    if (it != sessions.end()) {
        std::cout << "[SocketServer] Sending to " << peerId << std::endl;
    }
}

}