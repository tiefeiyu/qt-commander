#include <iostream>
#include <string>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <atomic>
#include <csignal>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <signal.h>
#endif

#include "mcp/transport.h"
#include "mcp/transport_stdio.h"
#include "mcp/transport_http.h"
#include "mcp/protocol.h"
#include "session/session_manager.h"
#include "common/socket_utils.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Global shutdown flag (set by signal handler, checked by main loop)
// ---------------------------------------------------------------------------
static std::atomic<bool> g_shutdown_requested{false};

#ifdef _WIN32
// Windows console control handler.
// On CTRL_CLOSE_EVENT we also close stdin to unblock the stdio reader thread.
static BOOL WINAPI consoleCtrlHandler(DWORD dwCtrlType) {
    switch (dwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        g_shutdown_requested = true;
        // Close stdin so the reader thread's blocking read unblocks.
        // This is a best-effort interrupt — if the transport uses a
        // thread reading stdin via std::getline, closing the file
        // descriptor will cause it to see EOF and exit.
        if (dwCtrlType == CTRL_CLOSE_EVENT) {
            _close(_fileno(stdin));
        }
        return TRUE;
    default:
        return FALSE;
    }
}
#else
// POSIX signal handler — sets the shutdown flag.
// The stdio transport's blocking read will return EINTR (since we
// install the handler without SA_RESTART), allowing the reader
// thread to check running_ and exit.
static void signalHandler(int /*sig*/) {
    g_shutdown_requested = true;
}
#endif

// ---------------------------------------------------------------------------
// Default workspace path
// ---------------------------------------------------------------------------
static fs::path default_workspace() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata && appdata[0] != '\0') {
        return fs::path(appdata) / "qt-commander";
    }
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile && userprofile[0] != '\0') {
        return fs::path(userprofile) / ".qt-commander";
    }
    return fs::path(".") / ".qt-commander";
#else
    const char* home = std::getenv("HOME");
    std::string home_str;
    if (home && home[0] != '\0') {
        home_str = home;
    } else {
        struct passwd* pw = getpwuid(getuid());
        if (pw && pw->pw_dir) {
            home_str = pw->pw_dir;
        }
    }
    if (!home_str.empty()) {
        return fs::path(home_str) / ".qt-commander";
    }
    return fs::path(".") / ".qt-commander";
#endif
}

// ---------------------------------------------------------------------------
// Forward declaration of tool registration (defined below main).
// ---------------------------------------------------------------------------
void registerTools(MCPProtocol& protocol, SessionManager& sessions);

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    fs::path workspace = default_workspace();
    std::string transport_type = "stdio";
    uint16_t http_port = 0;

    // ---- Parse CLI arguments ----
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--workspace" && i + 1 < argc) {
            workspace = fs::absolute(argv[++i]);
        } else if (arg == "--transport" && i + 1 < argc) {
            transport_type = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            http_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--help") {
            std::cout << "qt-commander MCP Server\n"
                      << "  --workspace <path>     Workspace directory"
                      << " (default: ~/.qt-commander)\n"
                      << "  --transport stdio|http Transport mode"
                      << " (default: stdio)\n"
                      << "  --port <N>             HTTP port (default: 0 = auto)\n"
                      << "  --help                 Show this help\n";
            return 0;
        }
    }

    // ---- Create workspace directory structure ----
    std::error_code ec;
    fs::create_directories(workspace / "builds", ec);
    fs::create_directories(workspace / "sessions", ec);
    fs::create_directories(workspace / "logs", ec);
    if (ec) {
        std::cerr << "[qt-commander] Failed to create workspace directories: "
                  << ec.message() << "\n";
        return 1;
    }

    // ---- Initialize socket subsystem (WinSock on Windows) ----
    if (!socket_init()) {
        std::cerr << "[qt-commander] socket_init failed\n";
        return 1;
    }

    // ---- Create transport ----
    std::unique_ptr<MCPTransport> transport;
    if (transport_type == "http") {
        transport = std::make_unique<HttpTransport>(http_port);
    } else {
        transport = std::make_unique<StdioTransport>();
    }

    // ---- Create protocol handler ----
    MCPProtocol protocol;

    // ---- Create session manager ----
    SessionManager sessions(workspace);

    // ---- Wire up MCP handlers ----
    protocol.setToolCallHandler([&](const std::string& name, const json& params) -> json {
        return sessions.handleToolCall(name, params);
    });
    protocol.setResourceListHandler([&]() -> json {
        return sessions.listResources();
    });
    protocol.setResourceReadHandler([&](const std::string& uri) -> json {
        return sessions.readResource(uri);
    });

    // ---- Register all tools ----
    registerTools(protocol, sessions);

    // ---- Install signal handlers ----
#ifdef _WIN32
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#else
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // No SA_RESTART — let blocking calls return with EINTR.
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    // Ignore SIGPIPE so we don't crash on broken TCP connections.
    signal(SIGPIPE, SIG_IGN);
#endif

    // ---- Start transport ----
    if (!transport->start([&](const MCPMessage& msg) {
        // Ignore messages after shutdown has been requested.
        if (g_shutdown_requested) return;

        std::string response = protocol.processMessage(msg.data);
        if (!response.empty()) {
            transport->send(MCPMessage{response});
        }
    })) {
        std::cerr << "[qt-commander] Failed to start transport\n";
        socket_cleanup();
        return 1;
    }

    std::cout << "[qt-commander] Server started.\n"
              << "  Workspace: " << workspace.string() << "\n"
              << "  Transport: " << transport_type;
    if (transport_type == "http") {
        auto* http = dynamic_cast<HttpTransport*>(transport.get());
        if (http) {
            std::cout << "\n  Port: " << http->port();
        }
    }
    std::cout << std::endl;

    // ---- Main loop: poll shutdown flag ----
    // The transport runs its own blocking loops on a separate thread.
    // Here we just sleep and check the shutdown flag periodically.
    while (!g_shutdown_requested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // ---- Graceful shutdown ----
    std::cout << "[qt-commander] Shutting down..." << std::endl;
    transport->stop();

    // Clean up all sessions.
    // (SessionManager destructor handles this; we just need to destroy it
    //  while the transport is already stopped.)

    std::cout << "[qt-commander] Goodbye." << std::endl;

    socket_cleanup();
    return 0;
}

// ===========================================================================
// Tool Registration
// ===========================================================================
void registerTools(MCPProtocol& protocol, SessionManager& /*sessions*/) {
    // sessions is reserved for future tool implementations that may need
    // to query session state during registration. For now, all dispatch
    // goes through the SessionManager::handleToolCall path.
    // -- Session management --
    protocol.registerTool({
        "qt_list_processes",
        "List running Qt processes that may be attachable",
        {{"type", "object"}, {"properties", json::object()},
         {"required", json::array()}}
    });

    protocol.registerTool({
        "qt_attach",
        "Inject the helper library into a running Qt process and open a session",
        {{"type", "object"},
         {"properties", {
             {"pid", {{"type", "integer"}, {"description", "Target process PID"}}}
         }},
         {"required", json::array({"pid"})}}
    });

    protocol.registerTool({
        "qt_detach",
        "Disconnect from a Qt process session",
        {{"type", "object"},
         {"properties", {
             {"session_id", {{"type", "string"},
                             {"description", "Session identifier to close"}}},
             {"purge", {{"type", "boolean"},
                        {"description", "Also purge session artifacts"}}}
         }},
         {"required", json::array({"session_id"})}}
    });

    protocol.registerTool({
        "qt_list_sessions",
        "List all active sessions",
        {{"type", "object"}, {"properties", json::object()},
         {"required", json::array()}}
    });

    // -- UI Inspection --
    protocol.registerTool({
        "qt_snapshot",
        "Take a full snapshot of the UI widget tree for the session",
        {{"type", "object"},
         {"properties", {
             {"session_id", {{"type", "string"},
                             {"description", "Active session ID"}}},
             {"include_hidden", {{"type", "boolean"},
                                 {"description", "Include hidden widgets"}}},
             {"detail", {{"type", "string"},
                         {"description", "Detail level: low|medium|high"}}}
         }},
         {"required", json::array({"session_id"})}}
    });

    protocol.registerTool({
        "qt_find_element",
        "Find UI elements matching a query in a session",
        {{"type", "object"},
         {"properties", {
             {"session_id", {{"type", "string"}}},
             {"query", {{"type", "object"},
                        {"description", "Query filter criteria"}}}
         }},
         {"required", json::array({"session_id", "query"})}}
    });

    protocol.registerTool({
        "qt_get_property",
        "Read a property from a UI element",
        {{"type", "object"},
         {"properties", {
             {"session_id", {{"type", "string"}}},
             {"element_id", {{"type", "integer"}}},
             {"name", {{"type", "string"},
                       {"description", "Property name to read"}}}
         }},
         {"required", json::array({"session_id", "element_id", "name"})}}
    });

    protocol.registerTool({
        "qt_set_property",
        "Write a property value on a UI element",
        {{"type", "object"},
         {"properties", {
             {"session_id", {{"type", "string"}}},
             {"element_id", {{"type", "integer"}}},
             {"name", {{"type", "string"}}},
             {"value", {}}
         }},
         {"required", json::array({"session_id", "element_id", "name", "value"})}}
    });

    protocol.registerTool({
        "qt_call_method",
        "Invoke a QMetaObject-invokable method on a UI element",
        {{"type", "object"},
         {"properties", {
             {"session_id", {{"type", "string"}}},
             {"element_id", {{"type", "integer"}}},
             {"method", {{"type", "string"}}},
             {"args", {{"type", "array"}, {"items", {}}}}
         }},
         {"required", json::array({"session_id", "element_id", "method"})}}
    });

    protocol.registerTool({
        "qt_screenshot",
        "Capture a screenshot of a UI element or the entire window",
        {{"type", "object"},
         {"properties", {
             {"session_id", {{"type", "string"}}},
             {"element_id", {{"type", "integer"},
                             {"description", "Optional element to screenshot"}}}
         }},
         {"required", json::array({"session_id"})}}
    });

    // -- Mouse / Keyboard / Touch --
    protocol.registerTool({
        "qt_mouse_click",
        "Send a mouse click to a UI element",
        {{"type", "object"},
         {"properties", {
             {"session_id", {{"type", "string"}}},
             {"element_id", {{"type", "integer"}}},
             {"button", {{"type", "string"},
                         {"enum", json::array({"left", "right", "middle"})}}},
             {"modifiers", {{"type", "array"}, {"items", {{"type", "string"}}}}}
         }},
         {"required", json::array({"session_id", "element_id"})}}
    });

    protocol.registerTool({
        "qt_keyboard_input",
        "Send keyboard input to a UI element",
        {{"type", "object"},
         {"properties", {
             {"session_id", {{"type", "string"}}},
             {"element_id", {{"type", "integer"}}},
             {"text", {{"type", "string"}}},
             {"modifiers", {{"type", "array"}, {"items", {{"type", "string"}}}}}
         }},
         {"required", json::array({"session_id", "element_id", "text"})}}
    });

    protocol.registerTool({
        "qt_focus",
        "Set focus on a UI element",
        {{"type", "object"},
         {"properties", {
             {"session_id", {{"type", "string"}}},
             {"element_id", {{"type", "integer"}}}
         }},
         {"required", json::array({"session_id", "element_id"})}}
    });

    // -- Build --
    protocol.registerTool({
        "qt_build_library",
        "Build the Qt injection library for the specified environment",
        {{"type", "object"},
         {"properties", {
             {"qt_env", {{"type", "string"},
                         {"description", "Path or name of Qt installation"}}},
             {"vcvars", {{"type", "string"},
                         {"description", "Path to vcvarsall.bat (Windows only)"}}},
             {"vcvars_args", {{"type", "string"},
                              {"description", "Extra args for vcvarsall"}}},
             {"arch", {{"type", "string"},
                       {"enum", json::array({"x86", "x64", "arm64"})}}},
             {"generator", {{"type", "string"},
                            {"description", "CMake generator"}}},
             {"qt_major_version", {{"type", "integer"},
                                   {"enum", json::array({5, 6})}}}
         }},
         {"required", json::array({"qt_env"})}}
    });
}
