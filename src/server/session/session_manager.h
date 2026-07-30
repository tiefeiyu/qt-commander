#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <random>
#include <filesystem>
#include <nlohmann/json.hpp>

#include "session.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

// Manages the lifecycle of all active Qt process sessions.
// Each session corresponds to one injected Qt process.
class SessionManager {
public:
    explicit SessionManager(const fs::path& workspace);
    ~SessionManager();

    // Non-copyable.
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    // ---- Tool dispatch ----
    // Called from the MCP protocol handler. Routes to the appropriate impl_*.
    json handleToolCall(const std::string& tool_name, const json& params);

    // ---- Resource handlers ----
    json listResources() const;
    json readResource(const std::string& uri) const;

    // ---- Session CRUD ----
    // Create a new session for the given PID. Returns the session ID.
    std::string createSession(int pid);

    // Destroy a session. If purge is true, also remove session artifacts.
    // Returns true if the session existed and was destroyed.
    bool destroySession(const std::string& session_id, bool purge = false);

    // Look up a session by ID. Returns nullptr if not found.
    Session* getSession(const std::string& session_id);

    // Access all sessions.
    const std::map<std::string, std::unique_ptr<Session>>& sessions() const {
        return sessions_;
    }

    // Workspace path.
    const fs::path& workspace() const { return workspace_; }

private:
    // ---- Helpers ----
    static std::string generateSessionId();
    static std::string generateToken();

    // ---- Tool implementations (Section 5 of design spec) ----

    // Session management
    json impl_list_processes();
    json impl_attach(const json& params);
    json impl_detach(const json& params);
    json impl_list_sessions();

    // UI Inspection
    json impl_snapshot(const json& params);
    json impl_find_element(const json& params);
    json impl_get_property(const json& params);
    json impl_set_property(const json& params);
    json impl_call_method(const json& params);
    json impl_screenshot(const json& params);

    // Interaction
    json impl_mouse_click(const json& params);
    json impl_keyboard_input(const json& params);
    json impl_focus(const json& params);

    // Build
    json impl_build_library(const json& params);

    // ---- State ----
    fs::path workspace_;
    std::map<std::string, std::unique_ptr<Session>> sessions_;
    std::map<int, std::string> pid_to_session_;  // Prevent duplicate PIDs

    // Random number generation for session IDs and tokens.
    std::mt19937 rng_;
};
