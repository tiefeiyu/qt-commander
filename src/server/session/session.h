#pragma once
#include <string>
#include <cstdint>
#include <memory>
#include <atomic>
#include <mutex>
#include <filesystem>
#include <nlohmann/json.hpp>

#include "../../common/socket_utils.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

// Metadata about an active session, suitable for serialization.
struct SessionInfo {
    std::string session_id;
    int pid = 0;
    bool connected = false;
    int snapshot_count = 0;
};

// Represents a single connection to a Qt process via the injected library.
// All TCP communication uses the length-prefixed frame protocol (framing.h).
class Session {
public:
    // Create a new session. Does NOT connect — call connect() separately.
    Session(const std::string& id, int pid,
            const fs::path& workspace, const std::string& token);

    // Destructor calls disconnect() if connected.
    ~Session();

    // Non-copyable, non-movable.
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    // Connect to the injected library at 127.0.0.1:port.
    // Sends an authentication frame and waits for a success response.
    // Returns true on successful handshake.
    bool connect(uint16_t port);

    // Send a shutdown notification to the target and close the TCP socket.
    // Safe to call multiple times.
    bool disconnect();

    // Returns true if the TCP connection is established and authenticated.
    bool isConnected() const { return connected_; }

    // Send a framed JSON-RPC request. Blocks until a response is received
    // or a 30-second timeout elapses. Returns the response JSON as a string.
    // Throws std::runtime_error on transport errors.
    std::string sendRpc(const std::string& method, const json& params);

    // ---- Accessors ----
    const std::string& id() const { return id_; }
    int pid() const { return pid_; }
    const fs::path& sessionDir() const { return session_dir_; }
    const std::string& token() const { return token_; }

    void incrementSnapshotCount() { snapshot_count_++; }
    int snapshotCount() const { return snapshot_count_; }

    // The TCP port the injected library listens on (set by connect()).
    uint16_t port() const { return port_; }

private:
    // Send raw framed data. Returns false on error.
    bool sendFrame(const std::vector<uint8_t>& data);

    // Receive a framed message. Returns empty vector on timeout / error.
    std::vector<uint8_t> recvFrame();

    // Build a JSON-RPC request string.
    std::string buildJsonRpc(const std::string& method,
                             const json& params, int req_id);

    std::string id_;
    int pid_;
    fs::path session_dir_;
    std::string token_;
    socket_t sock_ = INVALID_SOCK;
    bool connected_ = false;
    int snapshot_count_ = 0;
    uint16_t port_ = 0;
    std::atomic<int> request_counter_{0};
    mutable std::mutex sock_mutex_; // Guards socket operations.
};
