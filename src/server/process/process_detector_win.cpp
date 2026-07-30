#include "process_detector.h"
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>

#pragma comment(lib, "psapi.lib")

// ---------------------------------------------------------------------------
// Helper: wide-to-utf8 conversion
// ---------------------------------------------------------------------------
static std::string wideToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                   nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                         out.data(), len, nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------------------
// Helper: check loaded modules for QtCore DLL
// ---------------------------------------------------------------------------
struct QtModuleInfo {
    bool found{false};
    std::string version;   // "5" or "6"
    std::string dll_name;  // matched DLL filename
};

static QtModuleInfo checkQtModules(HANDLE hProcess) {
    QtModuleInfo info;

    DWORD needed = 0;
    // First call to get required buffer size
    if (!EnumProcessModules(hProcess, nullptr, 0, &needed))
        return info;

    std::vector<HMODULE> modules(needed / sizeof(HMODULE));
    if (!EnumProcessModules(hProcess, modules.data(),
                            static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                            &needed))
        return info;

    static const wchar_t* const qtDlls[] = {
        L"Qt5Core.dll", L"Qt5Cored.dll",
        L"Qt6Core.dll", L"Qt6Cored.dll"
    };

    for (const auto& hMod : modules) {
        wchar_t modName[MAX_PATH]{};
        if (GetModuleBaseNameW(hProcess, hMod, modName, MAX_PATH) == 0)
            continue;

        for (const auto* pattern : qtDlls) {
            if (_wcsicmp(modName, pattern) == 0) {
                info.found = true;
                info.dll_name = wideToUtf8(pattern);
                // Determine major version from the pattern
                if (wcsstr(pattern, L"Qt5"))
                    info.version = "5";
                else
                    info.version = "6";
                return info;
            }
        }
    }
    return info;
}

// ---------------------------------------------------------------------------
// Helper: get process window title via EnumWindows
// ---------------------------------------------------------------------------
struct FindWindowCtx {
    int pid;
    std::wstring title;
};

static BOOL CALLBACK enumWindowProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<FindWindowCtx*>(lParam);

    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    if (windowPid != static_cast<DWORD>(ctx->pid))
        return TRUE; // continue enumeration

    // Only consider visible top-level windows with a title
    if (!IsWindowVisible(hwnd))
        return TRUE;

    wchar_t buf[512]{};
    int len = GetWindowTextW(hwnd, buf, 512);
    if (len == 0)
        return TRUE;

    // Pick the first match (or could prefer the main window heuristically)
    ctx->title.assign(buf, static_cast<size_t>(len));
    return FALSE; // stop enumeration
}

static std::wstring getWindowTitle(int pid) {
    FindWindowCtx ctx{pid, {}};
    EnumWindows(enumWindowProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.title;
}

// ---------------------------------------------------------------------------
// Helper: get process architecture
// ---------------------------------------------------------------------------
static bool detectArch(HANDLE hProcess, std::string& arch, int& bitness) {
    BOOL isWow64 = FALSE;
    if (!IsWow64Process(hProcess, &isWow64))
        return false;

    if (isWow64) {
        // 32-bit process on 64-bit OS
        arch = "x86";
        bitness = 32;
        return true;
    }

    // Not WOW64 – could be native 64-bit or native 32-bit.
    // On a 64-bit OS, a non-WOW64 process is 64-bit.
    // On a 32-bit OS, a non-WOW64 process is 32-bit.
    SYSTEM_INFO sysInfo{};
    GetNativeSystemInfo(&sysInfo);

    switch (sysInfo.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
        arch = "x86_64";
        bitness = 64;
        break;
    case PROCESSOR_ARCHITECTURE_INTEL:
        arch = "x86";
        bitness = 32;
        break;
    case PROCESSOR_ARCHITECTURE_ARM64:
        arch = "arm64";
        bitness = 64;
        break;
    default:
        arch = "unknown";
        bitness = 0;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Public: listQtProcesses
// ---------------------------------------------------------------------------
std::vector<ProcessInfo> ProcessDetector::listQtProcesses() {
    std::vector<ProcessInfo> results;

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return results;

    PROCESSENTRY32W pe32{};
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            // Open the process for minimal query access
            HANDLE hProcess = OpenProcess(
                PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                FALSE, pe32.th32ProcessID);

            if (!hProcess) {
                // May be a system process we cannot open – skip silently.
                continue;
            }

            QtModuleInfo qtInfo = checkQtModules(hProcess);
            if (!qtInfo.found) {
                CloseHandle(hProcess);
                continue;
            }

            ProcessInfo info;
            info.pid = static_cast<int>(pe32.th32ProcessID);
            info.name = wideToUtf8(pe32.szExeFile);
            info.qt_version = qtInfo.version;

            // Window title
            std::wstring titleW = getWindowTitle(info.pid);
            info.title = wideToUtf8(titleW);

            // Architecture
            detectArch(hProcess, info.arch, info.bitness);

            CloseHandle(hProcess);
            results.push_back(std::move(info));
        } while (Process32NextW(hSnapshot, &pe32));
    }

    CloseHandle(hSnapshot);
    return results;
}

// ---------------------------------------------------------------------------
// Public: isQtProcess
// ---------------------------------------------------------------------------
bool ProcessDetector::isQtProcess(int pid) {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                   FALSE, static_cast<DWORD>(pid));
    if (!hProcess)
        return false;

    QtModuleInfo qtInfo = checkQtModules(hProcess);
    CloseHandle(hProcess);
    return qtInfo.found;
}

// ---------------------------------------------------------------------------
// Public: getProcessArch
// ---------------------------------------------------------------------------
bool ProcessDetector::getProcessArch(int pid, std::string& arch, int& bitness) {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE,
                                   static_cast<DWORD>(pid));
    if (!hProcess)
        return false;

    bool ok = detectArch(hProcess, arch, bitness);
    CloseHandle(hProcess);
    return ok;
}
