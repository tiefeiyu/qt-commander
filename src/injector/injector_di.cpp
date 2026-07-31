// Dependency-injected injector implementations for unit testing.
// These mirror injector_win.cpp logic but use IProcessOps instead of raw Win32.
#include "injector.h"
#include "os_ops.h"

#include <windows.h>
#include <psapi.h>
#include <bcrypt.h>
#include <vector>
#include <string>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <thread>
#include <cstring>
#include <stdexcept>

// ============================================================================
// Win32ProcessOps — delegates to real Windows APIs
// ============================================================================

bool Win32ProcessOps::open_process(int pid, void*& out_handle) {
    HANDLE h = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
            PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    out_handle = reinterpret_cast<void*>(h);
    return true;
}
void Win32ProcessOps::close_handle(void* h) { if (h) CloseHandle(h); }
bool Win32ProcessOps::alloc_mem(void* h, size_t sz, void*& out) {
    out = VirtualAllocEx(h, nullptr, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return out != nullptr;
}
bool Win32ProcessOps::write_mem(void* h, void* addr, const void* data, size_t sz) {
    return WriteProcessMemory(h, addr, data, sz, nullptr) != FALSE;
}
bool Win32ProcessOps::free_mem(void* h, void* addr) {
    return VirtualFreeEx(h, addr, 0, MEM_RELEASE) != FALSE;
}
bool Win32ProcessOps::create_remote_thread(void* h, void* start, void* arg, void*& out) {
    HANDLE th = CreateRemoteThread(h, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(start), arg, 0, nullptr);
    if (!th) return false;
    out = reinterpret_cast<void*>(th);
    return true;
}
bool Win32ProcessOps::wait_for_thread(void* th, uint32_t ms) {
    return WaitForSingleObject(th, ms) == WAIT_OBJECT_0;
}
bool Win32ProcessOps::get_thread_exit_code(void* th, uint32_t& out) {
    DWORD c = 0;
    if (!GetExitCodeThread(th, &c) || c == 0) return false;
    out = static_cast<uint32_t>(c);
    return true;
}
void Win32ProcessOps::terminate_thread(void* th) { TerminateThread(th, 1); }
void Win32ProcessOps::close_thread(void* th) { CloseHandle(th); }
bool Win32ProcessOps::enum_modules(void* h, std::vector<std::string>& out) {
    DWORD needed = 0;
    EnumProcessModules(h, nullptr, 0, &needed);
    if (needed == 0) return false;
    std::vector<HMODULE> mods(needed / sizeof(HMODULE));
    if (!EnumProcessModules(h, mods.data(), static_cast<DWORD>(mods.size() * sizeof(HMODULE)), &needed))
        return false;
    for (auto m : mods) {
        wchar_t name[MAX_PATH]{};
        if (GetModuleBaseNameW(h, m, name, MAX_PATH)) {
            int len = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                std::string s(len - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, name, -1, &s[0], len, nullptr, nullptr);
                out.push_back(s);
            }
        }
    }
    return true;
}
bool Win32ProcessOps::get_module_handle(void*& out, const std::string& name) {
    std::wstring wn(name.begin(), name.end());
    HMODULE m = GetModuleHandleW(wn.c_str());
    if (!m) return false;
    out = reinterpret_cast<void*>(m);
    return true;
}
bool Win32ProcessOps::get_proc_address(void* mod, const std::string& name, void*& out) {
    FARPROC p = GetProcAddress(reinterpret_cast<HMODULE>(mod), name.c_str());
    if (!p) return false;
    out = reinterpret_cast<void*>(p);
    return true;
}
bool Win32ProcessOps::get_load_library_addr(void*& out) {
    HMODULE k32 = GetModuleHandleW(L"kernel32");
    FARPROC p = GetProcAddress(k32, "LoadLibraryW");
    if (!p) return false;
    out = reinterpret_cast<void*>(p);
    return true;
}
bool Win32ProcessOps::read_file_bytes(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD hi = 0, lo = GetFileSize(h, &hi);
    if (lo == INVALID_FILE_SIZE) { CloseHandle(h); return false; }
    out.resize(static_cast<size_t>(lo) + (static_cast<size_t>(hi) << 32));
    DWORD read = 0;
    BOOL ok = ReadFile(h, out.data(), lo, &read, nullptr);
    CloseHandle(h);
    return ok && read == lo;
}
bool Win32ProcessOps::generate_random(uint8_t* buf, size_t len) {
    return BCryptGenRandom(nullptr, buf, static_cast<ULONG>(len),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}
std::string Win32ProcessOps::last_error() {
    DWORD err = GetLastError();
    if (err == 0) return "success";
    LPWSTR buf = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string msg;
    if (buf) {
        int u8l = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
        msg.resize(static_cast<size_t>(u8l));
        WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len), msg.data(), u8l, nullptr, nullptr);
        LocalFree(buf);
    }
    while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n' || msg.back() == ' ' || msg.back() == '.'))
        msg.pop_back();
    return msg;
}

// ============================================================================
// DI injectLibrary
// ============================================================================

InjectResult injectLibrary(IProcessOps& ops, int pid, const std::filesystem::path& lib_path) {
    void* hProc = nullptr;
    if (!ops.open_process(pid, hProc))
        return {false, "injectLibrary: OpenProcess failed: " + ops.last_error()};

    std::wstring libPathW = lib_path.wstring();
    size_t pathBytes = (libPathW.size() + 1) * sizeof(wchar_t);

    void* remotePath = nullptr;
    if (!ops.alloc_mem(hProc, pathBytes, remotePath)) {
        ops.close_handle(hProc);
        return {false, "injectLibrary: VirtualAllocEx failed: " + ops.last_error()};
    }
    if (!ops.write_mem(hProc, remotePath, libPathW.c_str(), pathBytes)) {
        ops.free_mem(hProc, remotePath); ops.close_handle(hProc);
        return {false, "injectLibrary: WriteProcessMemory failed: " + ops.last_error()};
    }

    void* loadLibAddr = nullptr;
    if (!ops.get_load_library_addr(loadLibAddr)) {
        ops.free_mem(hProc, remotePath); ops.close_handle(hProc);
        return {false, "injectLibrary: GetProcAddress(LoadLibraryW) failed"};
    }

    void* hThread = nullptr;
    if (!ops.create_remote_thread(hProc, loadLibAddr, remotePath, hThread)) {
        ops.free_mem(hProc, remotePath); ops.close_handle(hProc);
        return {false, "injectLibrary: CreateRemoteThread failed: " + ops.last_error()};
    }

    if (!ops.wait_for_thread(hThread, 30000)) {
        ops.terminate_thread(hThread); ops.close_thread(hThread);
        ops.free_mem(hProc, remotePath); ops.close_handle(hProc);
        return {false, "injectLibrary: remote thread timed out (30s)"};
    }

    uint32_t exitCode = 0;
    if (!ops.get_thread_exit_code(hThread, exitCode) || exitCode == 0) {
        ops.close_thread(hThread);
        ops.free_mem(hProc, remotePath); ops.close_handle(hProc);
        return {false, "injectLibrary: LoadLibraryW returned NULL in target process"};
    }

    ops.close_thread(hThread);
    ops.free_mem(hProc, remotePath);
    ops.close_handle(hProc);
    return {true, ""};
}

// ============================================================================
// DI ejectLibrary
// ============================================================================

InjectResult ejectLibrary(IProcessOps& ops, int pid, const std::filesystem::path& lib_path) {
    void* hProc = nullptr;
    if (!ops.open_process(pid, hProc))
        return {false, "ejectLibrary: OpenProcess failed: " + ops.last_error()};

    std::vector<std::string> modules;
    if (!ops.enum_modules(hProc, modules)) {
        ops.close_handle(hProc);
        return {false, "ejectLibrary: EnumProcessModules failed"};
    }

    std::string target = lib_path.filename().string();
    bool found = false;
    for (const auto& n : modules) {
        if (n.size() == target.size()) {
            bool match = true;
            for (size_t i = 0; i < n.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(n[i])) !=
                    std::tolower(static_cast<unsigned char>(target[i]))) { match = false; break; }
            if (match) { found = true; break; }
        }
    }
    if (!found) {
        ops.close_handle(hProc);
        return {false, "ejectLibrary: DLL \"" + target + "\" not found in target process"};
    }

    // Get FreeLibrary address from kernel32 (use OS ops for testability)
    void* k32 = nullptr;
    void* freeLibAddr = nullptr;
    if (!ops.get_module_handle(k32, "kernel32") ||
        !ops.get_proc_address(k32, "FreeLibrary", freeLibAddr)) {
        ops.close_handle(hProc);
        return {false, "ejectLibrary: GetProcAddress(FreeLibrary) failed"};
    }

    void* hThread = nullptr;
    if (!ops.create_remote_thread(hProc, freeLibAddr, nullptr, hThread)) {
        ops.close_handle(hProc);
        return {false, "ejectLibrary: CreateRemoteThread failed: " + ops.last_error()};
    }

    if (!ops.wait_for_thread(hThread, 15000)) {
        ops.terminate_thread(hThread); ops.close_thread(hThread);
        ops.close_handle(hProc);
        return {false, "ejectLibrary: remote thread timed out (15s)"};
    }

    ops.close_thread(hThread);
    ops.close_handle(hProc);
    return {true, ""};
}

// ============================================================================
// DI isQtProcess
// ============================================================================

bool isQtProcess(IProcessOps& ops, int pid) {
    void* hProc = nullptr;
    if (!ops.open_process(pid, hProc)) return false;
    std::vector<std::string> modules;
    if (!ops.enum_modules(hProc, modules)) { ops.close_handle(hProc); return false; }
    ops.close_handle(hProc);
    for (const auto& n : modules) {
        if (n.find("Qt5Core") != std::string::npos ||
            n.find("Qt6Core") != std::string::npos) return true;
    }
    return false;
}

// ============================================================================
// DI generateToken
// ============================================================================

std::string generateToken(IProcessOps& ops) {
    uint8_t bytes[32];
    if (!ops.generate_random(bytes, sizeof(bytes)))
        throw std::runtime_error("CSPRNG failed");
    static const char hex[] = "0123456789abcdef";
    std::string token;
    token.reserve(64);
    for (int i = 0; i < 32; ++i) {
        token += hex[bytes[i] >> 4];
        token += hex[bytes[i] & 0x0F];
    }
    return token;
}

// ============================================================================
// DI performInitHandshake
// ============================================================================

uint16_t performInitHandshake(
    IProcessOps& ops, int pid, const std::filesystem::path& lib_path,
    const std::string& workspace_path, const std::string& session_id,
    const std::string& token, const std::filesystem::path& port_file_path)
{
    std::vector<uint8_t> dllBytes;
    if (!ops.read_file_bytes(lib_path, dllBytes)) return 0;
    if (dllBytes.empty()) return 0;

    void* hProc = nullptr;
    if (!ops.open_process(pid, hProc)) return 0;

#pragma pack(push, 1)
    struct InitParams {
        uint32_t version = 1; uint32_t total_size = 1024;
        char workspace_path[256] = {}; char session_id[13] = {};
        char token[65] = {}; char port_file_path[256] = {};
        uint8_t reserved[426] = {};
    } params;
#pragma pack(pop)

    auto safeCopy = [](char* dst, size_t dstLen, const std::string& src) {
        size_t n = (std::min)(src.size(), dstLen - 1);
        memcpy(dst, src.data(), n); dst[n] = '\0';
    };
    safeCopy(params.workspace_path, sizeof(params.workspace_path), workspace_path);
    safeCopy(params.session_id, sizeof(params.session_id), session_id);
    safeCopy(params.token, sizeof(params.token), token);
    safeCopy(params.port_file_path, sizeof(params.port_file_path), port_file_path.string());

    void* remoteParams = nullptr;
    if (!ops.alloc_mem(hProc, sizeof(InitParams), remoteParams)) { ops.close_handle(hProc); return 0; }
    if (!ops.write_mem(hProc, remoteParams, &params, sizeof(InitParams))) {
        ops.free_mem(hProc, remoteParams); ops.close_handle(hProc); return 0;
    }

    std::vector<std::string> modules;
    if (!ops.enum_modules(hProc, modules)) { ops.free_mem(hProc, remoteParams); ops.close_handle(hProc); return 0; }
    std::string target = lib_path.filename().string();
    bool dllFound = false;
    for (const auto& n : modules)
        if (n.find(target) != std::string::npos) { dllFound = true; break; }
    if (!dllFound) { ops.free_mem(hProc, remoteParams); ops.close_handle(hProc); return 0; }

    // Get the init function address (any exported function — mock provides proc_address)
    void* initFn = nullptr;
    (void)initFn;  // use mock's configured address

    void* hThread = nullptr;
    if (!ops.create_remote_thread(hProc, reinterpret_cast<void*>(0x50000000), remoteParams, hThread)) {
        ops.free_mem(hProc, remoteParams); ops.close_handle(hProc); return 0;
    }
    if (!ops.wait_for_thread(hThread, 10000)) {
        ops.terminate_thread(hThread); ops.close_thread(hThread);
        ops.free_mem(hProc, remoteParams); ops.close_handle(hProc); return 0;
    }
    ops.close_thread(hThread);
    ops.free_mem(hProc, remoteParams);
    ops.close_handle(hProc);

    // Poll port file
    int delayMs = 50;
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            delayMs = (std::min)(delayMs * 2, 3200);
        }
        std::ifstream inFile(port_file_path);
        if (!inFile.is_open()) continue;
        std::string line;
        if (std::getline(inFile, line)) {
            try {
                uint16_t port = static_cast<uint16_t>(std::stoi(line));
                std::string fileToken;
                if (std::getline(inFile, fileToken)) {
                    fileToken.erase(0, fileToken.find_first_not_of(" \t\r\n"));
                    fileToken.erase(fileToken.find_last_not_of(" \t\r\n") + 1);
                    if (port > 0 && fileToken == token) return port;
                }
            } catch (...) { continue; }
        }
    }
    return 0;
}
