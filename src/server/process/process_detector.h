#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct ProcessInfo {
    int pid;
    std::string name;
    std::string title;       // Window title (best-effort)
    std::string qt_version;  // "5" or "6" (major version detected from DLL name)
    std::string arch;        // "x86_64", "x86", "arm64"
    int bitness;             // 32 or 64
};

class ProcessDetector {
public:
    // List all running Qt processes.
    static std::vector<ProcessInfo> listQtProcesses();

    // Check if a specific PID is a Qt process.
    static bool isQtProcess(int pid);

    // Get architecture info for a PID.
    static bool getProcessArch(int pid, std::string& arch, int& bitness);
};
