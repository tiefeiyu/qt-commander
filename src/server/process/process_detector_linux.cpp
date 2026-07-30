#include "process_detector.h"

#if defined(__linux__) || defined(__unix__)
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>

std::vector<ProcessInfo> ProcessDetector::listQtProcesses() {
    std::vector<ProcessInfo> result;
    DIR* dir = ::opendir("/proc");
    if (!dir) return result;
    struct dirent* entry;
    while ((entry = ::readdir(dir)) != nullptr) {
        int pid = std::atoi(entry->d_name);
        if (pid <= 0) continue;
        if (isQtProcess(pid)) {
            ProcessInfo info;
            info.pid = pid;
            std::ifstream comm("/proc/" + std::to_string(pid) + "/comm");
            std::getline(comm, info.name);
            info.title = "";
            std::string arch; int bitness;
            getProcessArch(pid, arch, bitness);
            info.arch = arch;
            info.bitness = bitness;
            result.push_back(info);
        }
    }
    ::closedir(dir);
    return result;
}

bool ProcessDetector::isQtProcess(int pid) {
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find("libQt") != std::string::npos && line.find("Core.so") != std::string::npos)
            return true;
    }
    return false;
}

bool ProcessDetector::getProcessArch(int pid, std::string& arch, int& bitness) {
    std::ifstream exe("/proc/" + std::to_string(pid) + "/exe", std::ios::binary);
    if (!exe) return false;
    unsigned char e_ident[5];
    exe.read(reinterpret_cast<char*>(e_ident), 5);
    if (e_ident[4] == 1) { arch = "x86"; bitness = 32; }
    else if (e_ident[4] == 2) { arch = "x86_64"; bitness = 64; }
    else if (e_ident[4] == 0xB7) { arch = "arm64"; bitness = 64; }
    else { arch = "unknown"; bitness = 0; }
    return true;
}

#else
// Stub for non-Linux platforms
#include <vector>
std::vector<ProcessInfo> ProcessDetector::listQtProcesses() { return {}; }
bool ProcessDetector::isQtProcess(int) { return false; }
bool ProcessDetector::getProcessArch(int, std::string&, int&) { return false; }
#endif
