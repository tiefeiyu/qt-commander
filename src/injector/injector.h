#pragma once
#include <string>
#include <cstdint>
#include <filesystem>

namespace fs = std::filesystem;

struct InjectResult {
    bool ok;
    std::string error;
};

// Inject a shared library into a running process using CreateRemoteThread + LoadLibrary.
InjectResult injectLibrary(int pid, const fs::path& lib_path);

// Eject (unload) a shared library from a process via EnumProcessModules + FreeLibrary.
InjectResult ejectLibrary(int pid, const fs::path& lib_path);

// Write InitParams into target process memory and call qt_commander_init.
// Returns the TCP port the library is listening on, or 0 on error.
uint16_t performInitHandshake(
    int pid,
    const fs::path& lib_path,
    const std::string& workspace_path,
    const std::string& session_id,
    const std::string& token,
    const fs::path& port_file_path
);

// Generate a cryptographically random 64-hex-character token.
std::string generateToken();

// Validate that the target PID is a Qt process by checking loaded modules.
bool isQtProcess(int pid);
