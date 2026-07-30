#include "session_manager.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <thread>
#include <stdexcept>

// ===========================================================================
// Construction / Destruction
// ===========================================================================

SessionManager::SessionManager(const fs::path& workspace)
    : workspace_(workspace)
    , rng_(std::random_device{}())
{
}

SessionManager::~SessionManager() {
    // Disconnect all sessions on shutdown.
    for (auto& [id, session] : sessions_) {
        (void)id;
        try {
            session->disconnect();
        } catch (...) {
            // Swallow errors during shutdown.
        }
    }
    sessions_.clear();
    pid_to_session_.clear();
}

// ===========================================================================
// ID / Token Generation
// ===========================================================================

std::string SessionManager::generateSessionId() {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    thread_local std::mt19937 gen(std::random_device{}());
    thread_local std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);

    std::string id;
    id.reserve(12);
    for (int i = 0; i < 12; ++i) {
        id += charset[dist(gen)];
    }
    return id;
}

std::string SessionManager::generateToken() {
    thread_local std::mt19937 gen(std::random_device{}());
    thread_local std::uniform_int_distribution<> dist(0, 255);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 32; ++i) {
        oss << std::setw(2) << dist(gen);
    }
    return oss.str();
}

// ===========================================================================
// Session CRUD
// ===========================================================================

std::string SessionManager::createSession(int pid) {
    // Check for duplicate PID.
    auto it = pid_to_session_.find(pid);
    if (it != pid_to_session_.end()) {
        // Session already exists for this PID.
        return it->second;
    }

    // Generate unique ID and auth token.
    std::string session_id;
    do {
        session_id = generateSessionId();
    } while (sessions_.find(session_id) != sessions_.end());

    std::string token = generateToken();

    // Create session directory.
    fs::path session_dir = workspace_ / "sessions" / session_id;
    std::error_code ec;
    fs::create_directories(session_dir, ec);
    fs::create_directories(session_dir / "snapshots", ec);
    fs::create_directories(session_dir / "screenshots", ec);

    // Create the session object.
    auto session = std::make_unique<Session>(session_id, pid, workspace_, token);
    sessions_[session_id] = std::move(session);
    pid_to_session_[pid] = session_id;

    std::cout << "[sessions] Created session " << session_id
              << " for PID " << pid << "\n";

    return session_id;
}

bool SessionManager::destroySession(const std::string& session_id, bool purge) {
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return false;
    }

    // Disconnect from the process.
    try {
        it->second->disconnect();
    } catch (const std::exception& e) {
        std::cerr << "[sessions] Error disconnecting " << session_id
                  << ": " << e.what() << "\n";
    }

    // Remove PID mapping.
    pid_to_session_.erase(it->second->pid());

    if (purge) {
        // Remove session directory and all artifacts.
        std::error_code ec;
        fs::remove_all(it->second->sessionDir(), ec);
        if (ec) {
            std::cerr << "[sessions] Failed to purge session dir for "
                      << session_id << ": " << ec.message() << "\n";
        }
    }

    sessions_.erase(it);
    std::cout << "[sessions] Destroyed session " << session_id << "\n";
    return true;
}

Session* SessionManager::getSession(const std::string& session_id) {
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return nullptr;
    }
    return it->second.get();
}

// ===========================================================================
// Tool Call Dispatch
// ===========================================================================

json SessionManager::handleToolCall(const std::string& tool_name, const json& params) {
    try {
        if (tool_name == "qt_list_processes") {
            return impl_list_processes();
        } else if (tool_name == "qt_attach") {
            return impl_attach(params);
        } else if (tool_name == "qt_detach") {
            return impl_detach(params);
        } else if (tool_name == "qt_list_sessions") {
            return impl_list_sessions();
        } else if (tool_name == "qt_snapshot") {
            return impl_snapshot(params);
        } else if (tool_name == "qt_find_element") {
            return impl_find_element(params);
        } else if (tool_name == "qt_get_property") {
            return impl_get_property(params);
        } else if (tool_name == "qt_set_property") {
            return impl_set_property(params);
        } else if (tool_name == "qt_call_method") {
            return impl_call_method(params);
        } else if (tool_name == "qt_screenshot") {
            return impl_screenshot(params);
        } else if (tool_name == "qt_mouse_click") {
            return impl_mouse_click(params);
        } else if (tool_name == "qt_keyboard_input") {
            return impl_keyboard_input(params);
        } else if (tool_name == "qt_focus") {
            return impl_focus(params);
        } else if (tool_name == "qt_build_library") {
            return impl_build_library(params);
        } else {
            return {{"error", "Unknown tool: " + tool_name},
                    {"error_code", -32601}};
        }
    } catch (const std::exception& e) {
        return {{"error", e.what()}, {"error_code", -32603}};
    }
}

// ===========================================================================
// Resource Handlers
// ===========================================================================

json SessionManager::listResources() const {
    json resources = json::array();

    for (const auto& [id, session] : sessions_) {
        // Add snapshot resources.
        fs::path snap_dir = session->sessionDir() / "snapshots";
        std::error_code ec;
        if (fs::exists(snap_dir, ec)) {
            for (const auto& entry : fs::directory_iterator(snap_dir, ec)) {
                if (entry.is_regular_file()) {
                    json r = {
                        {"uri", "session://" + id + "/snapshots/" +
                                entry.path().filename().string()},
                        {"mimeType", "application/json"},
                        {"name", entry.path().filename().string()}
                    };
                    resources.push_back(r);
                }
            }
        }

        // Add screenshot resources.
        fs::path ss_dir = session->sessionDir() / "screenshots";
        if (fs::exists(ss_dir, ec)) {
            for (const auto& entry : fs::directory_iterator(ss_dir, ec)) {
                if (entry.is_regular_file()) {
                    json r = {
                        {"uri", "session://" + id + "/screenshots/" +
                                entry.path().filename().string()},
                        {"mimeType", "image/png"},
                        {"name", entry.path().filename().string()}
                    };
                    resources.push_back(r);
                }
            }
        }
    }

    return resources;
}

json SessionManager::readResource(const std::string& uri) const {
    // Expected format: session://<session_id>/<type>/<filename>
    // Parse the URI.
    const std::string prefix = "session://";
    if (uri.find(prefix) != 0) {
        return {{"error", "Invalid resource URI: " + uri}};
    }

    std::string rest = uri.substr(prefix.size());
    auto first_slash = rest.find('/');
    if (first_slash == std::string::npos) {
        return {{"error", "Invalid resource URI: " + uri}};
    }

    std::string session_id = rest.substr(0, first_slash);
    std::string path_part = rest.substr(first_slash + 1);

    // Validate session exists.
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return {{"error", "Session not found: " + session_id}};
    }

    fs::path file_path = it->second->sessionDir() / path_part;

    std::error_code ec;
    if (!fs::exists(file_path, ec) || !fs::is_regular_file(file_path, ec)) {
        return {{"error", "Resource not found: " + uri}};
    }

    // Read file content and encode as base64 or plain text.
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {{"error", "Failed to open resource: " + uri}};
    }

    auto size = file.tellg();
    file.seekg(0);

    std::string content(static_cast<size_t>(size), '\0');
    file.read(&content[0], size);

    // Determine mime type.
    std::string ext = file_path.extension().string();
    std::string mime = "application/octet-stream";
    if (ext == ".json" || ext == ".txt") {
        mime = "application/json";
    } else if (ext == ".png") {
        mime = "image/png";
    } else if (ext == ".jpg" || ext == ".jpeg") {
        mime = "image/jpeg";
    }

    return {
        {"uri", uri},
        {"mimeType", mime},
        {"text", content}
    };
}

// ===========================================================================
// Tool Implementations
// ===========================================================================

// ---- Session Management ----

json SessionManager::impl_list_processes() {
    // PLACEHOLDER: Real implementation will use platform-specific process
    // enumeration (process_detector_win.cpp, process_detector_linux.cpp,
    // process_detector_macos.cpp).
    //
    // For now, return an empty list with a notification.
    json result = json::array();

    std::cout << "[sessions] qt_list_processes called (placeholder)\n";

    return {{
        {"type", "text"},
        {"text", json({{"processes", result},
                       {"note", "Process listing is a placeholder. "
                        "Use platform-specific enumeration once implemented."}}).dump()}
    }};
}

json SessionManager::impl_attach(const json& params) {
    int pid = params.value("pid", 0);
    if (pid <= 0) {
        return {{"error", "Invalid or missing 'pid' parameter"}};
    }

    // Check if we already have a session for this PID.
    auto pid_it = pid_to_session_.find(pid);
    if (pid_it != pid_to_session_.end()) {
        // Return existing session.
        const auto& existing = sessions_[pid_it->second];
        return {{
            {"type", "text"},
            {"text", json({
                {"session_id", existing->id()},
                {"pid", pid},
                {"connected", existing->isConnected()},
                {"note", "Session already exists for this PID"}
            }).dump()}
        }};
    }

    // Create a new session.
    std::string session_id = createSession(pid);
    Session* session = getSession(session_id);
    if (!session) {
        return {{"error", "Failed to create session"}};
    }

    // PLACEHOLDER: Real injection will:
    //   1. Determine the architecture and Qt version of the target process.
    //   2. Locate the matching injection library in workspace/builds/.
    //   3. Use platform-specific injection APIs (CreateRemoteThread on
    //      Windows, ptrace + dlopen on Linux, task_for_pid on macOS).
    //   4. The injected library opens a TCP listener on a random loopback
    //      port and waits for the server to connect.
    //   5. The server discovers the port (via shared memory / pipe).
    //   6. Call session->connect(port) to authenticate and establish RPC.
    //
    // For now, we return the session info and the token so the client can
    // observe the attachment flow.

    return {{
        {"type", "text"},
        {"text", json({
            {"session_id", session_id},
            {"pid", pid},
            {"token", session->token()},
            {"connected", session->isConnected()},
            {"note", "Session created. Injection is a placeholder — "
             "the library has not yet been injected. "
             "Use qt_build_library first if the injection library "
             "has not been compiled."}
        }).dump()}
    }};
}

json SessionManager::impl_detach(const json& params) {
    std::string session_id = params.value("session_id", "");
    bool purge = params.value("purge", false);

    if (session_id.empty()) {
        return {{"error", "Missing 'session_id' parameter"}};
    }

    if (destroySession(session_id, purge)) {
        return {{
            {"type", "text"},
            {"text", json({
                {"session_id", session_id},
                {"purged", purge},
                {"status", "detached"}
            }).dump()}
        }};
    }

    return {{"error", "Session not found: " + session_id}};
}

json SessionManager::impl_list_sessions() {
    json session_list = json::array();

    for (const auto& [id, session] : sessions_) {
        json info = {
            {"session_id", id},
            {"pid", session->pid()},
            {"connected", session->isConnected()},
            {"snapshot_count", session->snapshotCount()}
        };
        session_list.push_back(info);
    }

    return {{
        {"type", "text"},
        {"text", json({{"sessions", session_list}}).dump()}
    }};
}

// ---- UI Inspection ----

static Session* resolveSession(const json& params,
                               SessionManager& manager,
                               json& error_out) {
    std::string session_id = params.value("session_id", "");
    if (session_id.empty()) {
        error_out = {{"error", "Missing 'session_id' parameter"}};
        return nullptr;
    }

    Session* session = manager.getSession(session_id);
    if (!session) {
        error_out = {{"error", "Session not found: " + session_id}};
        return nullptr;
    }

    if (!session->isConnected()) {
        error_out = {{"error", "Session " + session_id + " is not connected"}};
        return nullptr;
    }

    return session;
}

json SessionManager::impl_snapshot(const json& params) {
    json err;
    Session* session = resolveSession(params, *this, err);
    if (!session) return err;

    try {
        bool include_hidden = params.value("include_hidden", false);
        std::string detail = params.value("detail", "medium");

        json rpc_params = {
            {"include_hidden", include_hidden},
            {"detail", detail}
        };

        std::string result = session->sendRpc("qt.snapshot", rpc_params);
        session->incrementSnapshotCount();

        // Save snapshot to disk.
        std::string filename = "snapshot_" +
            std::to_string(session->snapshotCount()) + ".json";
        fs::path snap_path = session->sessionDir() / "snapshots" / filename;

        std::ofstream snap_file(snap_path);
        if (snap_file) {
            snap_file << result;
        }

        return {{
            {"type", "text"},
            {"text", result}
        }};
    } catch (const std::exception& e) {
        return {{"error", std::string("Snapshot failed: ") + e.what()}};
    }
}

json SessionManager::impl_find_element(const json& params) {
    json err;
    Session* session = resolveSession(params, *this, err);
    if (!session) return err;

    if (!params.contains("query") || !params["query"].is_object()) {
        return {{"error", "Missing or invalid 'query' parameter"}};
    }

    try {
        json rpc_params = {{"query", params["query"]}};
        std::string result = session->sendRpc("qt.findElement", rpc_params);
        return {{"type", "text"}, {"text", result}};
    } catch (const std::exception& e) {
        return {{"error", std::string("find_element failed: ") + e.what()}};
    }
}

json SessionManager::impl_get_property(const json& params) {
    json err;
    Session* session = resolveSession(params, *this, err);
    if (!session) return err;

    int element_id = params.value("element_id", 0);
    std::string name = params.value("name", "");

    if (name.empty()) {
        return {{"error", "Missing 'name' parameter"}};
    }

    try {
        json rpc_params = {
            {"element_id", element_id},
            {"name", name}
        };
        std::string result = session->sendRpc("qt.getProperty", rpc_params);
        return {{"type", "text"}, {"text", result}};
    } catch (const std::exception& e) {
        return {{"error", std::string("get_property failed: ") + e.what()}};
    }
}

json SessionManager::impl_set_property(const json& params) {
    json err;
    Session* session = resolveSession(params, *this, err);
    if (!session) return err;

    int element_id = params.value("element_id", 0);
    std::string name = params.value("name", "");

    if (name.empty()) {
        return {{"error", "Missing 'name' parameter"}};
    }

    if (!params.contains("value")) {
        return {{"error", "Missing 'value' parameter"}};
    }

    try {
        json rpc_params = {
            {"element_id", element_id},
            {"name", name},
            {"value", params["value"]}
        };
        std::string result = session->sendRpc("qt.setProperty", rpc_params);
        return {{"type", "text"}, {"text", result}};
    } catch (const std::exception& e) {
        return {{"error", std::string("set_property failed: ") + e.what()}};
    }
}

json SessionManager::impl_call_method(const json& params) {
    json err;
    Session* session = resolveSession(params, *this, err);
    if (!session) return err;

    int element_id = params.value("element_id", 0);
    std::string method = params.value("method", "");

    if (method.empty()) {
        return {{"error", "Missing 'method' parameter"}};
    }

    try {
        json rpc_params = {
            {"element_id", element_id},
            {"method", method},
            {"args", params.value("args", json::array())}
        };
        std::string result = session->sendRpc("qt.callMethod", rpc_params);
        return {{"type", "text"}, {"text", result}};
    } catch (const std::exception& e) {
        return {{"error", std::string("call_method failed: ") + e.what()}};
    }
}

json SessionManager::impl_screenshot(const json& params) {
    json err;
    Session* session = resolveSession(params, *this, err);
    if (!session) return err;

    try {
        int element_id = params.value("element_id", 0);
        json rpc_params = {};
        if (element_id > 0) {
            rpc_params["element_id"] = element_id;
        }

        std::string result = session->sendRpc("qt.screenshot", rpc_params);

        // Save screenshot to disk.
        std::string filename = "screenshot_" +
            std::to_string(session->snapshotCount() + 1) + ".png";
        fs::path ss_path = session->sessionDir() / "screenshots" / filename;

        // The result is expected to be base64-encoded PNG data.
        std::ofstream ss_file(ss_path, std::ios::binary);
        if (ss_file) {
            // Parse the result — assume it's a JSON object with a "data" field
            // containing the base64 string.
            json parsed = json::parse(result);
            std::string base64_data = parsed.value("data", result);
            ss_file << base64_data;  // TODO: actual base64 decode.
        }

        return {{
            {"type", "text"},
            {"text", json({
                {"session_id", session->id()},
                {"screenshot_file", ss_path.string()}
            }).dump()}
        }};
    } catch (const std::exception& e) {
        return {{"error", std::string("screenshot failed: ") + e.what()}};
    }
}

// ---- Interaction ----

json SessionManager::impl_mouse_click(const json& params) {
    json err;
    Session* session = resolveSession(params, *this, err);
    if (!session) return err;

    try {
        json rpc_params = {
            {"element_id", params.value("element_id", 0)},
            {"button", params.value("button", "left")},
            {"modifiers", params.value("modifiers", json::array())}
        };
        std::string result = session->sendRpc("qt.mouseClick", rpc_params);
        return {{"type", "text"}, {"text", result}};
    } catch (const std::exception& e) {
        return {{"error", std::string("mouse_click failed: ") + e.what()}};
    }
}

json SessionManager::impl_keyboard_input(const json& params) {
    json err;
    Session* session = resolveSession(params, *this, err);
    if (!session) return err;

    std::string text = params.value("text", "");
    if (text.empty()) {
        return {{"error", "Missing 'text' parameter"}};
    }

    try {
        json rpc_params = {
            {"element_id", params.value("element_id", 0)},
            {"text", text},
            {"modifiers", params.value("modifiers", json::array())}
        };
        std::string result = session->sendRpc("qt.typeText", rpc_params);
        return {{"type", "text"}, {"text", result}};
    } catch (const std::exception& e) {
        return {{"error", std::string("keyboard_input failed: ") + e.what()}};
    }
}

json SessionManager::impl_focus(const json& params) {
    json err;
    Session* session = resolveSession(params, *this, err);
    if (!session) return err;

    try {
        json rpc_params = {
            {"element_id", params.value("element_id", 0)}
        };
        std::string result = session->sendRpc("qt.focus", rpc_params);
        return {{"type", "text"}, {"text", result}};
    } catch (const std::exception& e) {
        return {{"error", std::string("focus failed: ") + e.what()}};
    }
}

// ---- Build ----

json SessionManager::impl_build_library(const json& params) {
    std::string qt_env = params.value("qt_env", "");
    if (qt_env.empty()) {
        return {{"error", "Missing 'qt_env' parameter"}};
    }

    std::string vcvars = params.value("vcvars", "");
    std::string vcvars_args = params.value("vcvars_args", "");
    std::string arch = params.value("arch", "");
    std::string generator = params.value("generator", "");
    int qt_major_version = params.value("qt_major_version", 0);

    // PLACEHOLDER: Real implementation will:
    //   1. Locate the injection library source (src/library/).
    //   2. Configure CMake with the specified generator and Qt environment.
    //   3. Build the library using cmake --build.
    //   4. Copy the resulting binary to workspace/builds/.
    //
    // For now, return a descriptive message.

    std::cout << "[sessions] qt_build_library called (placeholder)\n"
              << "  qt_env: " << qt_env << "\n"
              << "  arch: " << arch << "\n"
              << "  generator: " << generator << "\n"
              << "  vcvars: " << vcvars << "\n";

    return {{
        {"type", "text"},
        {"text", json({
            {"status", "not_implemented"},
            {"message", "Library build is a placeholder. "
             "The build system will compile src/library/ with CMake "
             "using the specified Qt environment."},
            {"workspace_builds_dir", (workspace_ / "builds").string()}
        }).dump()}
    }};
}
