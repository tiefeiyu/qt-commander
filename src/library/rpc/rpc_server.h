#pragma once
#include <string>
#include "api.h"
#include "common/socket_utils.h"

// TCP JSON-RPC server entry point for the injected library.
// The single production implementation is the free function below,
// called by entry_win.cpp / entry_unix.cpp.
namespace qt_commander {

/// Run the JSON-RPC server loop on the calling (worker) thread until the
/// client disconnects or sends qt.shutdown.  On success (auth passed and
/// a session ran), returns 0.
int run_rpc_server(socket_t listen_fd,
                   std::string port_file_path,
                   const std::string& token,
                   const InitParams* params);

} // namespace qt_commander
