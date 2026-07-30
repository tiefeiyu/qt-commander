#include "transport_http.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

// ============================================================================
// Internal helpers
// ============================================================================

// Case-insensitive char comparison.
static bool iequal(char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
}

// Case-insensitive find within a string.
static size_t ifind(const std::string& haystack, const std::string& needle) {
    auto it = std::search(haystack.begin(), haystack.end(),
                          needle.begin(), needle.end(), iequal);
    return it == haystack.end() ? std::string::npos
                                : static_cast<size_t>(it - haystack.begin());
}

// Trim leading/trailing ASCII whitespace in-place.
static void trim(std::string& s) {
    auto not_space = [](char c) { return c != ' ' && c != '\t'; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
}

// Extract header value (case-insensitive lookup).
// Searches in |headers| (everything between request-line and \r\n\r\n).
static std::string get_header(const std::string& headers,
                              const std::string& name) {
    // Try with the leading \r\n first.
    std::string pattern = "\r\n" + name + ":";
    size_t pos = ifind(headers, pattern);
    if (pos == std::string::npos) {
        // Fallback: try at the very start of the header section.
        pattern = name + ":";
        if (ifind(headers, pattern) == 0) pos = 0;
    }
    if (pos == std::string::npos) return {};

    size_t start = headers.find(':', pos) + 1;
    while (start < headers.size() &&
           (headers[start] == ' ' || headers[start] == '\t')) {
        ++start;
    }
    size_t end = headers.find("\r\n", start);
    if (end == std::string::npos) end = headers.size();
    std::string val = headers.substr(start, end - start);
    trim(val);
    return val;
}

// ============================================================================
// HttpTransport  --  HTTP/SSE server on raw sockets
// ============================================================================

HttpTransport::HttpTransport(uint16_t port)
    : port_(port) {}

HttpTransport::~HttpTransport() {
    stop();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool HttpTransport::start(MessageHandler handler) {
    if (running_.exchange(true)) {
        return false; // Already running.
    }

    if (!socket_init()) {
        running_ = false;
        return false;
    }

    uint16_t bind_port = port_;
    listen_fd_ = tcp_listen_loopback(bind_port);
    if (listen_fd_ == INVALID_SOCK) {
        std::fprintf(stderr,
                     "[mcp:http] failed to bind on port %u\n",
                     static_cast<unsigned>(port_));
        running_ = false;
        return false;
    }
    port_ = bind_port; // Actual port (may differ from requested).

    server_thread_ = std::thread([this, cb = std::move(handler)]() {
        acceptLoop(cb);
    });

    return true;
}

bool HttpTransport::send(const MCPMessage& msg) {
    {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        // Bound the queue to prevent unbounded memory growth.
        if (send_queue_.size() >= 1024) {
            send_queue_.pop_front();
        }
        send_queue_.push_back(msg.data);
    }
    sse_cv_.notify_one();
    return true;
}

void HttpTransport::stop() {
    running_ = false;
    sse_cv_.notify_all();

    // Close the listen socket to unblock acceptLoop().
    if (listen_fd_ != INVALID_SOCK) {
        tcp_close(listen_fd_);
        listen_fd_ = INVALID_SOCK;
    }

    if (server_thread_.joinable()) {
        server_thread_.join();
    }

    // Close the SSE client socket (if one is connected).
    std::lock_guard<std::mutex> lock(sse_mutex_);
    if (sse_fd_ != INVALID_SOCK) {
        tcp_close(sse_fd_);
        sse_fd_ = INVALID_SOCK;
    }
    send_queue_.clear();
}

// ---------------------------------------------------------------------------
// Accept loop  (runs on server_thread_)
// ---------------------------------------------------------------------------

void HttpTransport::acceptLoop(MessageHandler handler) {
    while (running_) {
        socket_t client = tcp_accept(listen_fd_);
        if (!running_) break;
        if (client == INVALID_SOCK) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Each client gets its own detached thread so SSE connections don't
        // block the accept loop.
        std::thread([this, client, handler]() {
            handleClient(client, handler);
        }).detach();
    }
}

// ---------------------------------------------------------------------------
// Per-connection handler  (runs on a detached thread)
// ---------------------------------------------------------------------------

void HttpTransport::handleClient(socket_t client_fd, MessageHandler handler) {
    // 10-second receive timeout for reading the HTTP request line + headers.
    tcp_set_recv_timeout(client_fd, 10000);

    std::string method, path, body;
    if (!readHttpRequest(client_fd, method, path, body)) {
        sendHttpResponse(client_fd, 400, "Bad Request",
                         "text/plain", "Bad Request\n");
        tcp_close(client_fd);
        return;
    }

    // Route based on method + path.
    if (path == "/sse" || path == "/sse/") {
        if (method != "GET") {
            sendHttpResponse(client_fd, 405, "Method Not Allowed",
                             "text/plain", "Method Not Allowed\n");
        } else {
            handleSSE(client_fd);
            // handleSSE owns the socket; it closes it when done.
            return;
        }
    } else if (path == "/message" || path == "/message/") {
        if (method != "POST") {
            sendHttpResponse(client_fd, 405, "Method Not Allowed",
                             "text/plain", "Method Not Allowed\n");
        } else {
            handlePost(client_fd, body, handler);
        }
    } else if (path == "/health" || path == "/health/") {
        if (method != "GET") {
            sendHttpResponse(client_fd, 405, "Method Not Allowed",
                             "text/plain", "Method Not Allowed\n");
        } else {
            handleHealth(client_fd);
        }
    } else {
        sendNotFound(client_fd);
    }

    tcp_close(client_fd);
}

// ---------------------------------------------------------------------------
// SSE endpoint  (GET /sse)  -- long-lived connection
// ---------------------------------------------------------------------------

void HttpTransport::handleSSE(socket_t client_fd) {
    // Send SSE response headers.
    static const char sse_headers[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";

    if (!tcp_send_all(client_fd, sse_headers, sizeof(sse_headers) - 1)) {
        tcp_close(client_fd);
        return;
    }

    // Register this socket as the active SSE client.
    {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        // If an older SSE client is still connected, close it.
        if (sse_fd_ != INVALID_SOCK) {
            tcp_close(sse_fd_);
        }
        sse_fd_ = client_fd;
    }
    sse_cv_.notify_one();

    // Event loop: deliver queued messages as SSE data frames, with a
    // 30-second keepalive comment to keep middle-boxes happy.
    while (running_) {
        std::unique_lock<std::mutex> lock(sse_mutex_);
        bool has_work = sse_cv_.wait_for(lock, std::chrono::seconds(30),
                                          [this]() {
                                              return !send_queue_.empty() ||
                                                     !running_;
                                          });

        if (!running_) break;

        if (!has_work) {
            // Timeout -- send SSE keepalive comment.
            lock.unlock();
            static const char keepalive[] = ": keepalive\n\n";
            if (!tcp_send_all(client_fd, keepalive, sizeof(keepalive) - 1)) {
                break; // Client disconnected.
            }
            continue;
        }

        // Send all queued messages as SSE data: frames.
        while (!send_queue_.empty()) {
            std::string msg = std::move(send_queue_.front());
            send_queue_.pop_front();
            lock.unlock();

            std::string frame = "data: " + msg + "\n\n";
            if (!tcp_send_all(client_fd, frame.data(), frame.size())) {
                // Send failed -- client disconnected.
                {
                    std::lock_guard<std::mutex> l(sse_mutex_);
                    if (sse_fd_ == client_fd) sse_fd_ = INVALID_SOCK;
                    send_queue_.clear();
                }
                tcp_close(client_fd);
                return;
            }
            lock.lock();
        }
    }

    // Cleanup on exit.
    {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        if (sse_fd_ == client_fd) sse_fd_ = INVALID_SOCK;
    }
    tcp_close(client_fd);
}

// ---------------------------------------------------------------------------
// POST /message  endpoint
// ---------------------------------------------------------------------------

void HttpTransport::handlePost(socket_t client_fd, const std::string& body,
                               MessageHandler handler) {
    if (body.empty()) {
        sendHttpResponse(client_fd, 411, "Length Required",
                         "text/plain", "Content-Length required\n");
        return;
    }

    // Forward the JSON-RPC body to the registered message handler.
    try {
        MCPMessage msg;
        msg.data = body;
        handler(msg);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[mcp:http] handler threw: %s\n", e.what());
    }

    // Return 202 Accepted per the MCP HTTP/SSE specification.
    sendHttpResponse(client_fd, 202, "Accepted",
                     "application/json", "{}\n");
}

// ---------------------------------------------------------------------------
// GET /health  endpoint
// ---------------------------------------------------------------------------

void HttpTransport::handleHealth(socket_t client_fd) {
    static const char body[] = "{\"status\":\"ok\"}\n";
    sendHttpResponse(client_fd, 200, "OK", "application/json", body);
}

// ---------------------------------------------------------------------------
// 404 fallback
// ---------------------------------------------------------------------------

void HttpTransport::sendNotFound(socket_t client_fd) {
    static const char body[] = "Not Found\n";
    sendHttpResponse(client_fd, 404, "Not Found", "text/plain", body);
}

// ============================================================================
// HTTP protocol helpers
// ============================================================================

bool HttpTransport::readHttpRequest(socket_t fd, std::string& method,
                                    std::string& path, std::string& body) {
    // Read until \r\n\r\n (end of headers).  Cap at 64 KiB to avoid abuse.
    std::string header_buf;
    header_buf.reserve(1024);
    char c;

    while (header_buf.size() < 65536) {
        if (!tcp_recv_all(fd, &c, 1)) {
            return false; // Timeout or disconnect.
        }
        header_buf.push_back(c);
        if (header_buf.size() >= 4 &&
            header_buf.compare(header_buf.size() - 4, 4, "\r\n\r\n") == 0) {
            break;
        }
    }

    // Parse the request line:  "METHOD SP PATH SP HTTP/1.x"
    auto line_end = header_buf.find("\r\n");
    if (line_end == std::string::npos) return false;

    std::string request_line = header_buf.substr(0, line_end);

    auto space1 = request_line.find(' ');
    if (space1 == std::string::npos) return false;
    method = request_line.substr(0, space1);

    auto space2 = request_line.find(' ', space1 + 1);
    if (space2 == std::string::npos) return false;
    path = request_line.substr(space1 + 1, space2 - space1 - 1);

    // Parse Content-Length.
    std::string header_part = header_buf.substr(line_end + 2);
    std::string cl_str = get_header(header_part, "Content-Length");

    size_t content_length = 0;
    if (!cl_str.empty()) {
        char* end = nullptr;
        unsigned long parsed = std::strtoul(cl_str.c_str(), &end, 10);
        if (end == cl_str.c_str()) return false; // Not a valid number.
        content_length = static_cast<size_t>(parsed);
    }

    // Read the body.
    body.clear();
    if (content_length > 0) {
        tcp_set_recv_timeout(fd, 30000); // 30-second body timeout.
        body.resize(content_length);
        if (!tcp_recv_all(fd, &body[0], content_length)) {
            return false;
        }
    }

    return true;
}

void HttpTransport::sendHttpResponse(socket_t fd, int status,
                                     const char* status_text,
                                     const std::string& content_type,
                                     const std::string& body) {
    char status_line[80];
    std::snprintf(status_line, sizeof(status_line), "HTTP/1.1 %d %s\r\n",
                  status, status_text);

    std::string response;
    response.reserve(256 + body.size());
    response += status_line;
    response += "Content-Type: " + content_type + "\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Connection: close\r\n";
    response += "Access-Control-Allow-Origin: *\r\n";
    response += "\r\n";
    response += body;

    tcp_send_all(fd, response.data(), response.size());
}
