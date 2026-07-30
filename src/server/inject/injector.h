#pragma once
#include <string>
#include <cstdint>
#include <filesystem>

namespace fs = std::filesystem;

struct InjectResult {
    bool ok;
    std::string error;
};

// Inject a shared library into a running process.
// Returns result with error details on failure.
InjectResult injectLibrary(int pid, const fs::path& lib_path);

// Eject (unload) a shared library from a process. Must be called after shutdown RPC.
InjectResult ejectLibrary(int pid, const fs::path& lib_path);

// Write InitParams struct into target process memory and call qt_commander_init.
// Returns the TCP port the library is listening on (from port file), or 0 on error.
uint16_t performInitHandshake(int pid, const fs::path& lib_path,
                              const std::string& workspace_path,
                              const std::string& session_id,
                              const std::string& token,
                              const fs::path& port_file_path);
