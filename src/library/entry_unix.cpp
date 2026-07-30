// ---------------------------------------------------------------------------
// qt-commander -- POSIX (Linux / macOS) entry point
//
// A GCC/Clang `__attribute__((constructor))` sets an atomic flag only --
// no threads, no socket APIs, no heap allocations.
//
// qt_commander_init() does the real work: validates InitParams, initialises
// the socket subsystem (no-op on POSIX but keeps the API uniform), creates
// a listening socket, writes the port handshake file, and starts the RPC
// server background thread.
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

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
namespace {

/// Set to `true` once the library constructor runs.
std::atomic<bool> g_library_loaded{false};

/// Guards against double-initialisation via qt_commander_init.
std::atomic<bool> g_init_called{false};

/// Shared shutdown flag that the RPC server thread polls.
std::atomic<bool> g_shutdown_flag{false};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Forward declaration of the RPC server thread function
//
// Defined in rpc/rpc_server.cpp.  Blocks the calling thread for the lifetime
// of the RPC connection (accept -> auth -> dispatch -> close).  Closes
// `listen_fd` after accepting one connection.
// ---------------------------------------------------------------------------
namespace qt_commander {

void run_rpc_server(socket_t listen_fd,
                    std::string port_file_path,
                    std::string session_id,
                    std::string token,
                    std::atomic<bool>& shutdown_flag);

} // namespace qt_commander

// ---------------------------------------------------------------------------
// Library constructor / destructor
//
// `__attribute__((constructor))` is supported by GCC, Clang, and ICC.
// It runs when the shared object is loaded (dlopen / LD_PRELOAD).
//
// Like DllMain, it must NOT create threads, call socket APIs, allocate
// significant memory, or call most library functions -- the runtime may
// not be fully initialised yet.
// ---------------------------------------------------------------------------
__attribute__((constructor)) static void qt_commander_constructor()
{
    g_library_loaded.store(true, std::memory_order_release);
}

__attribute__((destructor)) static void qt_commander_destructor()
{
    // Signal the RPC thread to shut down.
    g_shutdown_flag.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// qt_commander_init  --  called by the injector after dlopen
// ---------------------------------------------------------------------------
extern "C" int qt_commander_init(const InitParams* params)
{
    // ---- guard against null / double-init ---------------------------------
    if (!params) {
        return -1;
    }

    bool expected = false;
    if (!g_init_called.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel)) {
        return -1;   // already initialised
    }

    // ---- validate InitParams ----------------------------------------------
    if (params->version != INIT_PARAMS_VERSION) {
        return -1;
    }
    if (params->total_size != INIT_PARAMS_TOTAL_SIZE) {
        return -1;
    }

    // ---- initialise socket subsystem (no-op on POSIX) ---------------------
    if (!socket_init()) {
        return -1;
    }

    // ---- create listening socket on loopback: random port ------------------
    uint16_t port = 0;
    socket_t listen_fd = tcp_listen_loopback(port);
    if (listen_fd == INVALID_SOCK) {
        return -1;
    }

    // ---- copy string fields -----------------------------------------------
    const std::string workspace_path(params->workspace_path);
    const std::string session_id(params->session_id);
    const std::string token(params->token);
    const std::string port_file_path(params->port_file_path);

    // ---- write port handshake file: "port\ntoken\n" ------------------------
    {
        std::ofstream ofs(port_file_path,
                          std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            tcp_close(listen_fd);
            return -1;
        }

        ofs << port << '\n';
        ofs << token << '\n';
        ofs.flush();

        if (!ofs) {
            tcp_close(listen_fd);
            std::remove(port_file_path.c_str());
            return -1;
        }
    }

    // ---- start the RPC server thread (detached) ---------------------------
    std::thread rpc_thread([listen_fd,
                            port_file_path,
                            session_id,
                            token]() mutable {
        qt_commander::run_rpc_server(listen_fd,
                                     std::move(port_file_path),
                                     std::move(session_id),
                                     std::move(token),
                                     g_shutdown_flag);
    });

    rpc_thread.detach();

    return 0;
}
