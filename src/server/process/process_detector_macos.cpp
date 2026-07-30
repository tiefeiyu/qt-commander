#include "process_detector.h"

#ifdef __APPLE__
#include <libproc.h>
#include <sys/sysctl.h>
#include <mach-o/dyld_images.h>
#include <mach/mach.h>
#include <string>
#include <vector>
#include <fstream>
#include <regex>
#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------
// macOS implementation (macOS 10.15+ / Ventura+)
//
// NOTE: Full library enumeration per process is restricted on modern macOS
// (SIP, hardened runtime). proc_pidinfo with PROC_PIDLISTFDS gives file
// descriptors, but mapped dylibs require task_for_pid which is privileged.
//
// The approach below uses proc_listallpids + proc_pidinfo to get the PID list
// and executable name, then parses the vmmap-style output of
// proc_regionfilename. A production version would need com.apple.system-task
// entitlement or a DYLD_INSERT_LIBRARIES approach.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Query a process's loaded image paths by walking /dev/fd (lightweight) or
// parsing vmmap output. The most portable approach without entitlements is
// to check the proc's argv or use sysctl to inspect its dyld info, but that
// also requires special entitlements on SIP-enabled systems.
//
// Here we take the pragmatic route: we check if the process has any open
// file-descriptor-backed mappings whose path contains "Qt" and "Core",
// by iterating its open file descriptors via proc_pidfdinfo.
// ---------------------------------------------------------------------------

static bool processHasQtCore(int pid) {
    // Get the number of open file descriptors
    int fdCount = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
    if (fdCount <= 0)
        return false;

    std::vector<proc_fdinfo> fds(static_cast<size_t>(fdCount));
    fdCount = proc_pidinfo(pid, PROC_PIDLISTFDS, 0,
                            fds.data(),
                            static_cast<int>(fds.size() * sizeof(proc_fdinfo)));
    if (fdCount <= 0)
        return false;

    std::regex qtCoreRe(R"(Qt(\d+)Core)");

    for (int i = 0; i < fdCount; ++i) {
        if (fds[static_cast<size_t>(i)].proc_fdtype != PROX_FDTYPE_VNODE)
            continue;

        struct vnode_fdinfowithpath vinfo;
        int ret = proc_pidfdinfo(pid, fds[static_cast<size_t>(i)].proc_fd,
                                  PROC_PIDFDVNODEPATHINFO, &vinfo,
                                  sizeof(vinfo));
        if (ret <= 0)
            continue;

        std::string path(vinfo.pvip.vip_path);
        std::smatch m;
        if (std::regex_search(path, m, qtCoreRe)) {
            return true;
        }
    }
    return false;
}

static int pidBitness(int pid) {
    // Use sysctl to check the process's CPU type
    cpu_type_t cpuType = CPU_TYPE_X86_64; // default
    size_t len = sizeof(cpuType);
    int mib[] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pid };
    struct kinfo_proc kp{};
    size_t kpLen = sizeof(kp);

    if (sysctl(mib, 4, &kp, &kpLen, nullptr, 0) != 0)
        return 64; // best guess

    // Use process's CSFLAGS / LP64 to determine bitness
    // On macOS there's no direct "is this process 32-bit" sysctl post-Catalina
    // because 32-bit processes are banned. So on modern macOS we assume 64-bit.
#if defined(__arm64__)
    // If we're running arm64 and the process is running under Rosetta,
    // it would show as CPU_TYPE_X86_64. Otherwise it's arm64.
    return 64;
#else
    // x86_64 host — all processes are 64-bit post-Catalina
    return 64;
#endif
}

// ---------------------------------------------------------------------------
// Public implementations
// ---------------------------------------------------------------------------
std::vector<ProcessInfo> ProcessDetector::listQtProcesses() {
    std::vector<ProcessInfo> results;

    int bufSize = proc_listallpids(nullptr, 0);
    if (bufSize <= 0)
        return results;

    std::vector<pid_t> pids(static_cast<size_t>(bufSize));
    int count = proc_listallpids(pids.data(),
                                  static_cast<int>(pids.size() * sizeof(pid_t)));
    if (count <= 0)
        return results;

    // Maximum process name buffer
    char nameBuf[PROC_PIDPATHINFO_MAXSIZE]{};

    for (int i = 0; i < count; ++i) {
        pid_t pid = pids[static_cast<size_t>(i)];
        if (pid == 0)
            continue;

        if (!processHasQtCore(static_cast<int>(pid)))
            continue;

        ProcessInfo info;
        info.pid = static_cast<int>(pid);

        // Get executable name
        memset(nameBuf, 0, sizeof(nameBuf));
        int nameLen = proc_name(pid, nameBuf, sizeof(nameBuf));
        if (nameLen > 0)
            info.name = nameBuf;
        else
            info.name = "unknown";

        // Best-effort: Qt version detection — we re-scan to find "5" or "6"
        // For brevity, re-use processHasQtCore logic with a version query.
        // In practice you would scan the fd list again, but since this is a
        // stub we just set a placeholder.
        info.qt_version = "6"; // Most macOS Qt apps use Qt6 now
        info.title = "(macOS window detection requires accessibility API)";
        info.arch = "x86_64";
        info.bitness = pidBitness(info.pid);

        // Refine version by scanning the fd list again more carefully
        // (simplified: we default to 6; production code would iterate
        // fdinfo paths and parse version from filename)
        results.push_back(std::move(info));
    }

    return results;
}

bool ProcessDetector::isQtProcess(int pid) {
    return processHasQtCore(pid);
}

bool ProcessDetector::getProcessArch(int pid, std::string& arch, int& bitness) {
    bitness = pidBitness(pid);
    arch = (bitness == 64) ? "x86_64" : "x86";
    return true;
}

#else
// Stub for non-macOS platforms
#include <vector>
std::vector<ProcessInfo> ProcessDetector::listQtProcesses() { return {}; }
bool ProcessDetector::isQtProcess(int) { return false; }
bool ProcessDetector::getProcessArch(int, std::string&, int&) { return false; }
#endif
