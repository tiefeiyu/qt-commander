#include "session.h"
#include "../../common/framing.h"

#include <iostream>
#include <sstream>
#include <vector>
#include <stdexcept>
#include <cstring>

// ---------------------------------------------------------------------------
// Timeout for receiving frame responses (milliseconds).
// ---------------------------------------------------------------------------
static const int SESSION_RECV_TIMEOUT_MS = 30000;

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------
Session::Session(const std::string& id, int pid,
                 const fs::path& workspace, const std::string& token)
    : id_(id)
    , pid_(pid)
    , token_(token)
{
    session_dir_ = workspace / "sessions" / id;
}

Session::~Session() {
    disconnect();
}

// ---------------------------------------------------------------------------
// connect
// ---------------------------------------------------------------------------
bool Session::connect(uint16_t port) {
    std::lock_guard<std::mutex> lock(sock_mutex_);

    if (connected_) {
        return true; // Already connected.
    }

    sock_ = tcp_connect_loopback(port);
    if (sock_ == INVALID_SOCK) {
        std::cerr << "[session] tcp_connect_loopback(" << port
                  << ") failed for session " << id_ << "\n";
        return false;
    }

    port_ = port;

    // Set receive timeout on the socket.
    tcp_set_recv_timeout(sock_, SESSION_RECV_TIMEOUT_MS);

    // Build authentication message: JSON-RPC request with the token.
    json auth_params = {{"token", token_}};
    std::string auth_msg = buildJsonRpc("qt.authenticate", auth_params, 0);

    // Encode and send the auth frame.
    auto frame = frame_encode(
        reinterpret_cast<const uint8_t*>(auth_msg.data()),
        auth_msg.size()
    );

    if (!tcp_send_all(sock_, frame.data(), frame.size())) {
        std::cerr << "[session] Failed to send auth frame for " << id_ << "\n";
        tcp_close(sock_);
        sock_ = INVALID_SOCK;
        return false;
    }

    // Read the response.
    auto raw_response = recvFrame();
    if (raw_response.empty()) {
        std::cerr << "[session] No auth response for " << id_ << "\n";
        tcp_close(sock_);
        sock_ = INVALID_SOCK;
        return false;
    }
    std::string response_str(raw_response.begin(), raw_response.end());

    // Parse the JSON-RPC response.
    try {
        json resp = json::parse(response_str);
        // Check for a result field — presence indicates success.
        if (resp.contains("result") && !resp.contains("error")) {
            connected_ = true;
            std::cout << "[session] Session " << id_
                      << " connected to PID " << pid_
                      << " on port " << port << "\n";
            return true;
        }

        // Check for explicit error.
        if (resp.contains("error")) {
            std::string err_msg = resp["error"].value("message", "unknown");
            std::cerr << "[session] Auth error for " << id_
                      << ": " << err_msg << "\n";
        }
    } catch (const json::parse_error& e) {
        std::cerr << "[session] Invalid auth response JSON: " << e.what() << "\n";
    }

    tcp_close(sock_);
    sock_ = INVALID_SOCK;
    return false;
}

// ---------------------------------------------------------------------------
// disconnect
// ---------------------------------------------------------------------------
bool Session::disconnect() {
    std::lock_guard<std::mutex> lock(sock_mutex_);

    if (!connected_ && sock_ == INVALID_SOCK) {
        return true; // Already disconnected.
    }

    // Send a shutdown notification to the injected library (best-effort).
    if (connected_ && sock_ != INVALID_SOCK) {
        json params = {{"reason", "server_shutdown"}};
        std::string shutdown_msg = buildJsonRpc("qt.shutdown", params, 0);

        auto frame = frame_encode(
            reinterpret_cast<const uint8_t*>(shutdown_msg.data()),
            shutdown_msg.size()
        );

        // Don't check return — the peer may already be gone.
        tcp_send_all(sock_, frame.data(), frame.size());
    }

    // Close the socket.
    if (sock_ != INVALID_SOCK) {
        tcp_close(sock_);
        sock_ = INVALID_SOCK;
    }

    connected_ = false;
    std::cout << "[session] Session " << id_ << " disconnected\n";
    return true;
}

// ---------------------------------------------------------------------------
// sendRpc
// ---------------------------------------------------------------------------
std::string Session::sendRpc(const std::string& method, const json& params) {
    std::lock_guard<std::mutex> lock(sock_mutex_);

    if (!connected_ || sock_ == INVALID_SOCK) {
        throw std::runtime_error("Session " + id_ +
                                 " is not connected (sendRpc: " + method + ")");
    }

    int req_id = ++request_counter_;
    std::string request_str = buildJsonRpc(method, params, req_id);

    // Encode and send.
    auto frame = frame_encode(
        reinterpret_cast<const uint8_t*>(request_str.data()),
        request_str.size()
    );

    if (!tcp_send_all(sock_, frame.data(), frame.size())) {
        connected_ = false;
        tcp_close(sock_);
        sock_ = INVALID_SOCK;
        throw std::runtime_error("Failed to send RPC " + method +
                                 " for session " + id_);
    }

    // Receive response.
    auto raw_frame = recvFrame();
    if (raw_frame.empty()) {
        connected_ = false;
        tcp_close(sock_);
        sock_ = INVALID_SOCK;
        throw std::runtime_error("Timeout or error receiving RPC response for " +
                                 method + " on session " + id_);
    }
    std::string response(raw_frame.begin(), raw_frame.end());

    // Validate the JSON-RPC response.
    try {
        json resp = json::parse(response);
        // Check for an error object.
        if (resp.contains("error")) {
            int code = resp["error"].value("code", 0);
            std::string msg = resp["error"].value("message", "unknown error");
            throw std::runtime_error("RPC error " + std::to_string(code) +
                                     ": " + msg);
        }
        // Return the result as a serialized string.
        if (resp.contains("result")) {
            return resp["result"].dump();
        }
        // Unexpected response shape — return the whole thing.
        return response;
    } catch (const json::parse_error& e) {
        throw std::runtime_error("Invalid JSON-RPC response: " +
                                 std::string(e.what()));
    }
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool Session::sendFrame(const std::vector<uint8_t>& data) {
    return tcp_send_all(sock_, data.data(), data.size());
}

std::vector<uint8_t> Session::recvFrame() {
    FrameDecoder decoder;
    uint8_t byte;

    while (true) {
        // Read one byte at a time from TCP
        if (!tcp_recv_all(sock_, &byte, 1)) {
            return {};  // Connection closed or error
        }

        std::vector<uint8_t> payload;
        auto result = decoder.feed(&byte, 1, payload);

        if (result == FrameResult::Error) {
            return {};
        }
        if (result == FrameResult::Complete) {
            return payload;
        }
        // NeedMore — continue reading
    }
}

std::string Session::buildJsonRpc(const std::string& method,
                                   const json& params, int req_id) {
    json msg = {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params}
    };
    // Notification if req_id is 0 (no response expected), otherwise a request.
    if (req_id != 0) {
        msg["id"] = req_id;
    }
    return msg.dump();
}
