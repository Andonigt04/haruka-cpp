#ifndef SOCKET_SERVER_H
#define SOCKET_SERVER_H

#include <boost/asio.hpp>
#include <memory>
#include <thread>
#include <map>
#include <functional>

namespace Haruka {

using boost::asio::ip::tcp;

/** @brief Connected TCP session wrapper. */
struct SocketSession {
    tcp::socket socket;
    std::string peerId;
    
    SocketSession(boost::asio::io_context& io_context)
        : socket(io_context) {}
};

/**
 * @brief Lightweight TCP socket server for message relay.
 */
class SocketServer {
public:
    /** @brief Constructs a server attached to an io_context and port. */
    SocketServer(boost::asio::io_context& io_context, int port);
    /** @brief Releases server resources. */
    ~SocketServer();
    
    /** @brief Starts accepting incoming connections. */
    void start();
    /** @brief Stops the server and closes sessions. */
    void stop();
    /** @brief Returns true while the server is running. */
    bool isRunning() const { return running; }
    
    /** @brief Broadcasts a message to all connected sessions. */
    void broadcastMessage(const std::string& message);
    /** @brief Sends a message to one peer by id. */
    void sendMessage(const std::string& peerId, const std::string& message);
    
    /** @brief Sets the incoming-message callback. */
    void setMessageCallback(std::function<void(const std::string&, const std::string&)> cb) {
        messageCallback = cb;
    }

private:
    boost::asio::io_context& io_context;
    std::unique_ptr<tcp::acceptor> acceptor;
    bool running = false;
    std::map<std::string, std::shared_ptr<SocketSession>> sessions;
    std::function<void(const std::string&, const std::string&)> messageCallback;
    
    /** @brief Begins an asynchronous accept operation. */
    void asyncAccept();
    /** @brief Handles one accepted connection result. */
    void handleAccept(std::shared_ptr<SocketSession> session,
                     const boost::system::error_code& error);
};

}

#endif