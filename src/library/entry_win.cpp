// ---------------------------------------------------------------------------
// qt-commander -- Windows entry point
//
// DllMain(DLL_PROCESS_ATTACH) sets an atomic flag only -- no threads, no
// socket APIs, no heap allocations (Windows loader lock restrictions).
//
// qt_commander_init() does the real work: validates InitParams, initialises
// Winsock, creates a listening socket, writes the port handshake file, and
// starts the RPC server background thread.
// ---------------------------------------------------------------------------

#include "api.h"
#include "../common/socket_utils.h"
#include "compat_qt.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
namespace {

/// Set to `true` once DllMain(DLL_PROCESS_ATTACH) runs.
std::atomic<bool> g_library_loaded{false};

/// Guards against double-initialisation via qt_commander_init.
std::atomic<bool> g_init_called{false};

/// Shared shutdown flag that the RPC server thread polls.
std::atomic<bool> g_shutdown_flag{false};

/// One-time Winsock initialiser (called from qt_commander_init, NOT DllMain).
/// socket_init() in socket_utils.cpp is safe to call multiple times but we
/// only call it once for clarity.
bool ensure_socket_init()
{
    static std::once_flag flag;
    bool ok = true;
    std::call_once(flag, [&ok]() { ok = socket_init(); });
    return ok;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Forward declaration of the RPC server thread function
//
// Defined in rpc/rpc_server.cpp.  This function blocks the calling thread
// for the lifetime of the RPC connection (accept → auth → dispatch → close).
// It closes `listen_fd` after accepting one connection.
// ---------------------------------------------------------------------------
namespace qt_commander {

void run_rpc_server(socket_t listen_fd,
                    std::string port_file_path,
                    std::string session_id,
                    std::string token,
                    std::atomic<bool>& shutdown_flag);

} // namespace qt_commander

// ---------------------------------------------------------------------------
// DllMain
// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)hinstDLL;
    (void)lpvReserved;

    if (fdwReason == DLL_PROCESS_ATTACH) {
        // Do NOT create threads, call socket APIs, or allocate memory here.
        // The loader lock is held and many operations are unsafe.
        g_library_loaded.store(true, std::memory_order_release);

        // Disable thread-attach/detach notifications for performance.
        DisableThreadLibraryCalls(hinstDLL);
    }

    return TRUE;
}

// ---------------------------------------------------------------------------
// qt_commander_init  --  called by the injector after LoadLibrary
// ---------------------------------------------------------------------------
// DEBUG: incremental enable to find ACCESS_VIOLATION root cause
extern "C" int qt_commander_init(const InitParams* params)
{
    // ---- guard against null / double-init ---------------------------------
    if (!params) return -1;

    bool expected = false;
    if (!g_init_called.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return -1;

    // Validate InitParams
    if (params->version != INIT_PARAMS_VERSION) return -1;
    if (params->total_size != INIT_PARAMS_TOTAL_SIZE) return -1;

    // Init Winsock
    if (!ensure_socket_init()) return -1;

    // Create listening socket
    uint16_t port = 0;
    socket_t listen_fd = tcp_listen_loopback(port);
    if (listen_fd == INVALID_SOCK) return -1;

    // Copy string fields
    std::string ws(params->workspace_path);
    std::string sid(params->session_id);
    std::string tok(params->token);
    std::string pf(params->port_file_path);

    // Write port file
    {
        std::ofstream ofs(pf, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) { tcp_close(listen_fd); return -1; }
        ofs << port << '\n' << tok << '\n';
        ofs.flush();
        if (!ofs) { tcp_close(listen_fd); std::remove(pf.c_str()); return -1; }
    }

    // Start RPC thread
    std::thread rpc_thread([listen_fd, pf, sid, tok]() mutable {
        qt_commander::run_rpc_server(listen_fd, std::move(pf), std::move(sid), std::move(tok), g_shutdown_flag);
    });
    rpc_thread.detach();

    return 0;
}
