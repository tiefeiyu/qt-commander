#pragma once
#include "transport.h"
#include "../../common/socket_utils.h"

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <cstdint>
#include <string>

// HTTP/SSE transport. Listens on 127.0.0.1:port.
//   SSE endpoint:  GET /sse  -> stream of events
//   Message endpoint:  POST /message  -> JSON-RPC request body
//   Health:   GET /health -> 200 OK
class HttpTransport : public MCPTransport {
public:
    explicit HttpTransport(uint16_t port = 0);
    ~HttpTransport() override;

    bool start(MessageHandler handler) override;
    bool send(const MCPMessage& msg) override;
    void stop() override;

    // The actual port the server is listening on (0 = OS-assigned).
    uint16_t port() const { return port_; }

private:
    // Listener thread: accepts connections and dispatches to handler threads.
    void acceptLoop(MessageHandler handler);

    // Handle one accepted client connection (called from a detached thread).
    void handleClient(socket_t client_fd, MessageHandler handler);

    // Per-endpoint handlers.
    void handleSSE(socket_t client_fd);
    void handlePost(socket_t client_fd, const std::string& body,
                   MessageHandler handler);
    void handleHealth(socket_t client_fd);
    void sendNotFound(socket_t client_fd);

    // Helper: parse one HTTP request from a connected socket.
    // Returns false on error (bad request / timeout).
    static bool readHttpRequest(socket_t fd, std::string& method,
                                std::string& path, std::string& body);

    // Helper: send a complete HTTP response and close.
    static void sendHttpResponse(socket_t fd, int status,
                                 const char* status_text,
                                 const std::string& content_type,
                                 const std::string& body);

    uint16_t port_;
    socket_t listen_fd_ = INVALID_SOCK;
    std::atomic<bool> running_{false};
    std::thread server_thread_;

    // SSE client state (guarded by sse_mutex_).
    std::mutex sse_mutex_;
    std::condition_variable sse_cv_;
    std::deque<std::string> send_queue_;
    socket_t sse_fd_ = INVALID_SOCK;
};
