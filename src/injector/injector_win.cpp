#define NOMINMAX
#include "injector.h"

#include <windows.h>
#include <psapi.h>
#include <bcrypt.h>
#ifdef _MSC_VER
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "bcrypt.lib")
#endif
#include <vector>
#include <string>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <thread>
#include <cstring>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static std::string lastErrorString() {
    DWORD err = GetLastError();
    if (err == 0)
        return "success";

    LPWSTR buf = nullptr;
    DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buf), 0, nullptr);

    std::string msg;
    if (buf) {
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, buf,
                                           static_cast<int>(len),
                                           nullptr, 0, nullptr, nullptr);
        msg.resize(static_cast<size_t>(utf8Len));
        WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len),
                             msg.data(), utf8Len, nullptr, nullptr);
        LocalFree(buf);
    }

    // Trim trailing whitespace / \r\n / '.'
    while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n' ||
                            msg.back() == ' ' || msg.back() == '.'))
        msg.pop_back();

    return "error " + std::to_string(err) + ": " + msg;
}

// ---------------------------------------------------------------------------
// PE export directory parser -- locate an exported function's RVA from a DLL
// file image loaded into memory.
// ---------------------------------------------------------------------------

static bool readFileBytes(const fs::path& path, std::vector<uint8_t>& out) {
    out.clear();
    HANDLE hFile = CreateFileW(path.wstring().c_str(), GENERIC_READ,
                                FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    DWORD sizeHigh = 0;
    DWORD sizeLow = GetFileSize(hFile, &sizeHigh);
    if (sizeLow == INVALID_FILE_SIZE) {
        CloseHandle(hFile);
        return false;
    }

    out.resize(static_cast<size_t>(sizeLow) +
               (static_cast<size_t>(sizeHigh) << 32));

    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hFile, out.data(), sizeLow, &bytesRead, nullptr);
    CloseHandle(hFile);
    return ok && bytesRead == sizeLow;
}

// Walk section headers to map a PE relative virtual address (RVA) to a file
// offset.  Returns `rva` as fallback when no section covers it.
static DWORD rvaToOffset(const IMAGE_NT_HEADERS* nt, DWORD rva) {
    const IMAGE_SECTION_HEADER* sect = IMAGE_FIRST_SECTION(nt);
    WORD count = nt->FileHeader.NumberOfSections;
    for (WORD i = 0; i < count; ++i) {
        if (rva >= sect[i].VirtualAddress &&
            rva < sect[i].VirtualAddress + sect[i].Misc.VirtualSize) {
            return sect[i].PointerToRawData + (rva - sect[i].VirtualAddress);
        }
    }
    return rva; // fallback (flat layout or misaligned)
}

// Read a 2-byte or 4-byte value at a file offset.
static WORD readWord(const uint8_t* base, DWORD offset) {
    WORD v;
    memcpy(&v, base + offset, 2);
    return v;
}

static DWORD readDword(const uint8_t* base, DWORD offset) {
    DWORD v;
    memcpy(&v, base + offset, 4);
    return v;
}

// Find the RVA of an exported function by name.  Returns 0 if not found.
static DWORD findExportRva(const std::vector<uint8_t>& dllBytes,
                           const std::string& exportName) {
    if (dllBytes.size() < sizeof(IMAGE_DOS_HEADER))
        return 0;

    const auto* dos =
        reinterpret_cast<const IMAGE_DOS_HEADER*>(dllBytes.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;

    DWORD ntOffset = dos->e_lfanew;
    if (dllBytes.size() < static_cast<size_t>(ntOffset) + sizeof(IMAGE_NT_HEADERS))
        return 0;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        dllBytes.data() + ntOffset);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;

    // Export directory
    const IMAGE_DATA_DIRECTORY& expDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (expDir.Size == 0 || expDir.VirtualAddress == 0)
        return 0;

    const uint8_t* base = dllBytes.data();
    DWORD expDirOffset = rvaToOffset(nt, expDir.VirtualAddress);
    const auto* exp =
        reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + expDirOffset);

    DWORD namesOffset = rvaToOffset(nt, exp->AddressOfNames);
    DWORD ordinalsOffset = rvaToOffset(nt, exp->AddressOfNameOrdinals);
    DWORD functionsOffset = rvaToOffset(nt, exp->AddressOfFunctions);

    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        DWORD nameRva = readDword(base, namesOffset + i * 4);
        DWORD nameOffset = rvaToOffset(nt, nameRva);
        const char* namePtr =
            reinterpret_cast<const char*>(base + nameOffset);

        if (exportName == namePtr) {
            WORD ordinal = readWord(base, ordinalsOffset + i * 2);
            DWORD funcRva = readDword(
                base, functionsOffset + static_cast<DWORD>(ordinal) * 4);
            return funcRva;
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------
// InitParams -- must match src/library/api.h exactly (1024-byte fixed layout)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct InitParams {
    uint32_t version;           // offset 0
    uint32_t total_size;        // offset 4
    char workspace_path[256];   // offset 8
    char session_id[13];        // offset 264
    char token[65];             // offset 277
    char port_file_path[256];   // offset 342
    uint8_t reserved[426];      // offset 598 -> total 1024
};
#pragma pack(pop)
static_assert(sizeof(InitParams) == 1024, "InitParams size must be 1024 bytes");

// ---------------------------------------------------------------------------
// injectLibrary
// ---------------------------------------------------------------------------
InjectResult injectLibrary(int pid, const fs::path& lib_path) {
    // 1. Open target process with minimal required rights
    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
            PROCESS_QUERY_INFORMATION,
        FALSE, static_cast<DWORD>(pid));
    if (!hProcess) {
        return {false, "injectLibrary: OpenProcess failed for PID " +
                           std::to_string(pid) + ": " + lastErrorString()};
    }

    // 2. Convert DLL path to wide string
    std::wstring libPathW = lib_path.wstring();
    size_t pathBytes = (libPathW.size() + 1) * sizeof(wchar_t);

    // 3. Allocate memory in target for the path string
    void* remotePath = VirtualAllocEx(hProcess, nullptr, pathBytes,
                                       MEM_COMMIT | MEM_RESERVE,
                                       PAGE_READWRITE);
    if (!remotePath) {
        CloseHandle(hProcess);
        return {false, "injectLibrary: VirtualAllocEx failed: " +
                           lastErrorString()};
    }

    // 4. Write DLL path into target memory
    if (!WriteProcessMemory(hProcess, remotePath, libPathW.c_str(), pathBytes,
                            nullptr)) {
        VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return {false, "injectLibrary: WriteProcessMemory failed: " +
                           lastErrorString()};
    }

    // 5. Get address of LoadLibraryW in kernel32
    HMODULE kernel32 = GetModuleHandleW(L"kernel32");
    if (!kernel32) {
        VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return {false, "injectLibrary: GetModuleHandle(kernel32) failed: " +
                           lastErrorString()};
    }

    FARPROC loadLibAddr = GetProcAddress(kernel32, "LoadLibraryW");
    if (!loadLibAddr) {
        VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return {false, "injectLibrary: GetProcAddress(LoadLibraryW) failed: " +
                           lastErrorString()};
    }

    // 6. Create remote thread that calls LoadLibraryW(path)
    HANDLE hThread = CreateRemoteThread(
        hProcess, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibAddr),
        remotePath, 0, nullptr);
    if (!hThread) {
        VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return {false, "injectLibrary: CreateRemoteThread failed: " +
                           lastErrorString()};
    }

    // 7. Wait for thread to finish
    DWORD waitResult = WaitForSingleObject(hThread, 30000);
    if (waitResult == WAIT_TIMEOUT) {
        TerminateThread(hThread, 1);
        CloseHandle(hThread);
        VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return {false, "injectLibrary: remote thread timed out (30 s)"};
    }
    if (waitResult == WAIT_FAILED) {
        CloseHandle(hThread);
        VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return {false, "injectLibrary: WaitForSingleObject failed: " +
                           lastErrorString()};
    }

    // 8. Get the HMODULE (DLL base) from the thread exit code
    DWORD exitCode = 0;
    if (!GetExitCodeThread(hThread, &exitCode) || exitCode == 0) {
        CloseHandle(hThread);
        VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return {false,
                "injectLibrary: LoadLibraryW returned NULL in target process"};
    }

    // 9. Clean up
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    return {true, ""};
}

// ---------------------------------------------------------------------------
// performInitHandshake
// ---------------------------------------------------------------------------
uint16_t performInitHandshake(int pid, const fs::path& lib_path,
                              const std::string& workspace_path,
                              const std::string& session_id,
                              const std::string& token,
                              const fs::path& port_file_path) {
    // 1. Read DLL from disk and find the RVA of qt_commander_init
    std::vector<uint8_t> dllBytes;
    if (!readFileBytes(lib_path, dllBytes)) {
        return 0;
    }

    DWORD initRva = findExportRva(dllBytes, "qt_commander_init");
    if (initRva == 0) {
        return 0;
    }

    // 2. Open the target process
    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
            PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        FALSE, static_cast<DWORD>(pid));
    if (!hProcess) {
        return 0;
    }

    // 3. Lookup DLL base via module enumeration
    std::wstring libNameW = lib_path.filename().wstring();
    HMODULE dllBase = nullptr;

    DWORD needed = 0;
    EnumProcessModules(hProcess, nullptr, 0, &needed);
    std::vector<HMODULE> modules(needed / sizeof(HMODULE));
    if (!EnumProcessModules(
            hProcess, modules.data(),
            static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
            &needed)) {
        CloseHandle(hProcess);
        return 0;
    }

    for (const auto& hMod : modules) {
        wchar_t modName[MAX_PATH]{};
        if (GetModuleBaseNameW(hProcess, hMod, modName, MAX_PATH) == 0)
            continue;
        if (_wcsicmp(modName, libNameW.c_str()) == 0) {
            dllBase = hMod;
            break;
        }
    }

    if (!dllBase) {
        CloseHandle(hProcess);
        return 0;
    }

    // 4. Compute entry-point address in the target
    void* initAddr = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(dllBase) +
        static_cast<uintptr_t>(initRva));

    // 5. Fill InitParams
    InitParams params{};
    params.version = 1;
    params.total_size = 1024;

    auto safeCopy = [](char* dst, size_t dstLen, const std::string& src) {
        size_t n = (std::min)(src.size(), dstLen - 1);
        memcpy(dst, src.data(), n);
        dst[n] = '\0';
    };

    safeCopy(params.workspace_path, sizeof(params.workspace_path),
             workspace_path);
    safeCopy(params.session_id, sizeof(params.session_id), session_id);
    safeCopy(params.token, sizeof(params.token), token);
    safeCopy(params.port_file_path, sizeof(params.port_file_path),
             port_file_path.string());

    // 6. Allocate memory in target for InitParams
    void* remoteParams = VirtualAllocEx(hProcess, nullptr, sizeof(InitParams),
                                         MEM_COMMIT | MEM_RESERVE,
                                         PAGE_READWRITE);
    if (!remoteParams) {
        CloseHandle(hProcess);
        return 0;
    }

    // 7. Write InitParams into target
    if (!WriteProcessMemory(hProcess, remoteParams, &params,
                            sizeof(InitParams), nullptr)) {
        VirtualFreeEx(hProcess, remoteParams, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return 0;
    }

    // 8. Create remote thread calling qt_commander_init(params)
    HANDLE hThread = CreateRemoteThread(
        hProcess, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(initAddr),
        remoteParams, 0, nullptr);
    if (!hThread) {
        VirtualFreeEx(hProcess, remoteParams, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return 0;
    }

    // 9. Wait for init call to finish (it should copy the params and return
    //    quickly)
    DWORD waitResult = WaitForSingleObject(hThread, 10000);
    if (waitResult != WAIT_OBJECT_0) {
        // Init timed out or failed -- clean up and return.
        TerminateThread(hThread, 1);
        CloseHandle(hThread);
        VirtualFreeEx(hProcess, remoteParams, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return 0;
    }
    CloseHandle(hThread);

    // 10. Free the remote params allocation
    VirtualFreeEx(hProcess, remoteParams, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    // 11. Poll the port file with exponential backoff
    int delayMs = 50;
    uint16_t port = 0;
    std::string fileToken;

    for (int attempt = 0; attempt < 10; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(delayMs));
            delayMs = (std::min)(delayMs * 2, 3200);
        }

        std::ifstream inFile(port_file_path);
        if (!inFile.is_open())
            continue;

        std::string line;
        if (std::getline(inFile, line)) {
            try {
                port = static_cast<uint16_t>(std::stoi(line));
            } catch (...) {
                port = 0;
                continue;
            }
        }

        if (std::getline(inFile, fileToken)) {
            // Trim whitespace
            auto trim = [](std::string& s) {
                s.erase(0, s.find_first_not_of(" \t\r\n"));
                s.erase(s.find_last_not_of(" \t\r\n") + 1);
            };
            trim(fileToken);
        }

        if (port > 0 && !fileToken.empty())
            break;
    }

    // 12. Verify token
    if (port == 0 || fileToken != token)
        return 0;

    return port;
}

// ---------------------------------------------------------------------------
// ejectLibrary
// ---------------------------------------------------------------------------
InjectResult ejectLibrary(int pid, const fs::path& lib_path) {
    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        FALSE, static_cast<DWORD>(pid));
    if (!hProcess) {
        return {false, "ejectLibrary: OpenProcess failed: " + lastErrorString()};
    }

    // Enumerate modules to find the DLL base
    DWORD needed = 0;
    EnumProcessModules(hProcess, nullptr, 0, &needed);
    std::vector<HMODULE> modules(needed / sizeof(HMODULE));
    if (!EnumProcessModules(hProcess, modules.data(),
                            static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                            &needed)) {
        CloseHandle(hProcess);
        return {false, "ejectLibrary: EnumProcessModules failed: " + lastErrorString()};
    }

    std::wstring libNameW = lib_path.filename().wstring();
    HMODULE dllBase = nullptr;
    for (const auto& hMod : modules) {
        wchar_t modName[MAX_PATH]{};
        if (GetModuleBaseNameW(hProcess, hMod, modName, MAX_PATH) &&
            _wcsicmp(modName, libNameW.c_str()) == 0) {
            dllBase = hMod;
            break;
        }
    }
    if (!dllBase) {
        CloseHandle(hProcess);
        return {false, "ejectLibrary: DLL not found in target process"};
    }

    // Call FreeLibrary in the target
    FARPROC freeLibAddr = GetProcAddress(
        GetModuleHandleW(L"kernel32"), "FreeLibrary");
    if (!freeLibAddr) {
        CloseHandle(hProcess);
        return {false, "ejectLibrary: GetProcAddress(FreeLibrary) failed"};
    }

    HANDLE hThread = CreateRemoteThread(
        hProcess, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(freeLibAddr),
        dllBase, 0, nullptr);
    if (!hThread) {
        CloseHandle(hProcess);
        return {false, "ejectLibrary: CreateRemoteThread failed: " + lastErrorString()};
    }

    DWORD waitResult = WaitForSingleObject(hThread, 15000);
    if (waitResult != WAIT_OBJECT_0) {
        TerminateThread(hThread, 1);
        CloseHandle(hThread);
        CloseHandle(hProcess);
        return {false, "ejectLibrary: remote thread timed out (15s)"};
    }
    CloseHandle(hThread);
    CloseHandle(hProcess);
    return {true, ""};
}

// ---------------------------------------------------------------------------
// isQtProcess
// ---------------------------------------------------------------------------
bool isQtProcess(int pid) {
    HANDLE hProcess = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, static_cast<DWORD>(pid));
    if (!hProcess) return false;

    HMODULE mods[1024];
    DWORD needed = 0;
    bool found = false;
    if (EnumProcessModules(hProcess, mods, sizeof(mods), &needed)) {
        int count = needed / sizeof(HMODULE);
        for (int i = 0; i < count && !found; ++i) {
            wchar_t name[MAX_PATH]{};
            if (GetModuleBaseNameW(hProcess, mods[i], name, MAX_PATH)) {
                std::wstring wn(name);
                found = (wn.find(L"Qt5Core") != std::wstring::npos ||
                         wn.find(L"Qt6Core") != std::wstring::npos);
            }
        }
    }
    CloseHandle(hProcess);
    return found;
}

// ---------------------------------------------------------------------------
// generateToken
// ---------------------------------------------------------------------------
std::string generateToken() {
    uint8_t bytes[32];  // 32 bytes -> 64 hex chars
    NTSTATUS status = BCryptGenRandom(
        nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) {
        throw std::runtime_error("BCryptGenRandom failed");
    }
    static const char hex[] = "0123456789abcdef";
    std::string token;
    token.reserve(64);
    for (int i = 0; i < 32; ++i) {
        token += hex[bytes[i] >> 4];
        token += hex[bytes[i] & 0x0F];
    }
    // Zero the raw bytes after use
    SecureZeroMemory(bytes, sizeof(bytes));
    return token;
}
