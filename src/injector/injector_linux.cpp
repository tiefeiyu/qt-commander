// ---------------------------------------------------------------------------
// qt-commander Linux injector — ptrace-based shared-library injection.
// ---------------------------------------------------------------------------
#ifdef __linux__

#include "injector.h"
#include "ptrace_ops.h"
#include "elf_parser.h"

#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/user.h>
#include <sys/uio.h>
#include <dlfcn.h>
#include <elf.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <algorithm>
#include <unistd.h>
#include <signal.h>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string lastErrorStr() {
    return std::string(std::strerror(errno)) + " (errno=" +
           std::to_string(errno) + ")";
}

// Find dlopen address in the target process.
// Strategy: find libdl/libc base in target via /proc/<pid>/maps,
// then find the offset of "dlopen" in the same .so on disk,
// and add base + offset to get the target's dlopen address.
static uintptr_t findDlopenInTarget(int pid) {
    // 1. Find libdl or libc in target maps.
    //    On modern glibc, dlopen is in libdl.so.2 or libc.so.6.
    uintptr_t base = ptrace_ops::find_library_base(pid, "libdl.so.2");
    fs::path libPath;
    if (base != 0) {
        // Find the actual path from maps.
        std::vector<ptrace_ops::MapEntry> maps;
        if (ptrace_ops::read_maps(pid, maps)) {
            for (const auto& m : maps) {
                if (m.path.find("libdl") != std::string::npos &&
                    m.start == base) {
                    libPath = m.path;
                    break;
                }
            }
        }
    }
    if (base == 0 || libPath.empty()) {
        // Try libc (dlopen is often a weak alias in libc).
        base = ptrace_ops::find_library_base(pid, "libc.so.6");
        if (base != 0) {
            std::vector<ptrace_ops::MapEntry> maps;
            if (ptrace_ops::read_maps(pid, maps)) {
                for (const auto& m : maps) {
                    if (m.path.find("libc") != std::string::npos &&
                        m.path.find("libc.so") != std::string::npos &&
                        m.start == base) {
                        libPath = m.path;
                        break;
                    }
                }
            }
        }
    }

    if (base == 0 || libPath.empty()) return 0;

    // 2. Parse the disk ELF to find "dlopen" offset.
    //    Try "__libc_dlopen_mode" first (glibc internal), then "dlopen".
    uintptr_t offset = findElfExportOffset(libPath, "__libc_dlopen_mode");
    if (offset == 0) {
        offset = findElfExportOffset(libPath, "dlopen");
    }
    if (offset == 0) return 0;

    return base + offset;
}

// ---------------------------------------------------------------------------
// injectLibrary (Linux ptrace version)
// ---------------------------------------------------------------------------

InjectResult injectLibrary(int pid, const fs::path& lib_path) {
    fs::path absLib = fs::absolute(lib_path).lexically_normal();
    std::string libPathStr = absLib.string();

    // 1. Check ptrace permissions.
    if (!ptrace_ops::can_attach(pid)) {
        return {false, std::string{"injectLibrary: cannot attach to PID "} +
                       std::to_string(pid) + ": " + ptrace_ops::last_error_str()};
    }

    // 2. Attach to the target.
    if (!ptrace_ops::attach(pid)) {
        return {false, std::string{"injectLibrary: attach failed: "} +
                       ptrace_ops::last_error_str()};
    }

    // 3. Find dlopen address in target.
    uintptr_t dlopenAddr = findDlopenInTarget(pid);
    if (dlopenAddr == 0) {
        ptrace_ops::detach(pid);
        return {false, "injectLibrary: cannot find dlopen in target process"};
    }

    // 4. Allocate a scratch page for the remote_call trampoline.
    //    remote mmap via syscall.
    //    mmap(0, 0x1000, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
    uintptr_t scratch = 0;
    if (!ptrace_ops::remote_syscall(pid,
            /*__NR_mmap=*/9,
            0, 0x1000,
            PROT_READ | PROT_WRITE | PROT_EXEC,
            MAP_PRIVATE | MAP_ANONYMOUS,
            static_cast<uintptr_t>(-1), 0,
            scratch) || scratch == 0 ||
        scratch == static_cast<uintptr_t>(-1)) {
        ptrace_ops::detach(pid);
        return {false, "injectLibrary: remote mmap failed"};
    }

    // Write int3 (0xCC) at the scratch page (used as return trap).
    uint8_t int3 = 0xCC;
    ptrace_ops::write_mem(pid, scratch, &int3, 1);

    // 5. Allocate memory for the library path string.
    //    mmap(0, pathSize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
    size_t pathSize = libPathStr.size() + 1;
    pathSize = (pathSize + 0xFFF) & ~static_cast<size_t>(0xFFF); // page-align
    uintptr_t remotePath = 0;
    if (!ptrace_ops::remote_syscall(pid, 9, 0, pathSize,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            static_cast<uintptr_t>(-1), 0,
            remotePath) || remotePath == 0 ||
        remotePath == static_cast<uintptr_t>(-1)) {
        // Free scratch.
        ptrace_ops::remote_syscall(pid, 11, scratch, 0x1000, 0, 0, 0, 0, remotePath);
        ptrace_ops::detach(pid);
        return {false, "injectLibrary: remote mmap (path) failed"};
    }

    // 6. Write the library path into target memory.
    if (!ptrace_ops::write_mem(pid, remotePath, libPathStr.c_str(),
                                libPathStr.size() + 1)) {
        ptrace_ops::remote_syscall(pid, 11, remotePath, pathSize,
                                    0, 0, 0, 0, remotePath);
        ptrace_ops::remote_syscall(pid, 11, scratch, 0x1000,
                                    0, 0, 0, 0, scratch);
        ptrace_ops::detach(pid);
        return {false, "injectLibrary: write_mem (path) failed"};
    }

    // 7. Call dlopen(path, RTLD_NOW) in the target.
    //    RTLD_NOW = 2
    uintptr_t dlopenRet = 0;
    // remote_call can only pass one argument.  But dlopen needs 2.
    // We use remote_syscall for a one-arg wrapper or use a different approach.
    //
    // Option A: call __libc_dlopen_mode(path, RTLD_NOW) — requires finding
    //   that symbol which we already did.
    //   __libc_dlopen_mode takes (const char* name, int mode).
    // Option B: inject a small trampoline that does:
    //   mov rdi, path
    //   mov rsi, RTLD_NOW
    //   call dlopen
    //   int3
    //
    // We use approach B: write a trampoline to the scratch page.
    // The trampoline (x86-64 machine code):
    //   48 BF <path>      movabs rdi, path (10 bytes)
    //   48 BE 02000000..  movabs rsi, RTLD_NOW (10 bytes)
    //   48 B8 <dlopen>    movabs rax, dlopen (10 bytes)
    //   FF D0             call rax (2 bytes)
    //   CC                int3 (1 byte)
    // Total: 33 bytes

    std::vector<uint8_t> trampoline;
    // movabs rdi, path
    trampoline.push_back(0x48);
    trampoline.push_back(0xBF);
    for (int i = 0; i < 8; ++i)
        trampoline.push_back(static_cast<uint8_t>((remotePath >> (i * 8)) & 0xFF));

    // movabs rsi, RTLD_NOW
    trampoline.push_back(0x48);
    trampoline.push_back(0xBE);
    uint64_t rtld_now = RTLD_NOW;
    for (int i = 0; i < 8; ++i)
        trampoline.push_back(static_cast<uint8_t>((rtld_now >> (i * 8)) & 0xFF));

    // movabs rax, dlopenAddr
    trampoline.push_back(0x48);
    trampoline.push_back(0xB8);
    for (int i = 0; i < 8; ++i)
        trampoline.push_back(static_cast<uint8_t>((dlopenAddr >> (i * 8)) & 0xFF));

    // call rax
    trampoline.push_back(0xFF);
    trampoline.push_back(0xD0);

    // int3
    trampoline.push_back(0xCC);

    // Write trampoline to scratch page.
    if (!ptrace_ops::write_mem(pid, scratch, trampoline.data(),
                                trampoline.size())) {
        ptrace_ops::remote_syscall(pid, 11, remotePath, pathSize,
                                    0, 0, 0, 0, remotePath);
        ptrace_ops::remote_syscall(pid, 11, scratch, 0x1000,
                                    0, 0, 0, 0, scratch);
        ptrace_ops::detach(pid);
        return {false, "injectLibrary: write trampoline failed"};
    }

    // Save registers.
    user_regs_struct savedRegs;
    if (!ptrace_ops::get_regs(pid, &savedRegs, sizeof(savedRegs))) {
        ptrace_ops::remote_syscall(pid, 11, remotePath, pathSize,
                                    0, 0, 0, 0, remotePath);
        ptrace_ops::remote_syscall(pid, 11, scratch, 0x1000,
                                    0, 0, 0, 0, scratch);
        ptrace_ops::detach(pid);
        return {false, "injectLibrary: get_regs failed"};
    }

    // Set RIP to trampoline start and continue.
    uintptr_t savedRip = savedRegs.rip;
    savedRegs.rip = scratch;
    if (!ptrace_ops::set_regs(pid, &savedRegs, sizeof(savedRegs))) {
        ptrace_ops::detach(pid);
        return {false, "injectLibrary: set_regs failed"};
    }

    // Continue execution.
    errno = 0;
    if (ptrace(PTRACE_CONT, pid, nullptr, nullptr) == -1) {
        ptrace_ops::detach(pid);
        return {false, "injectLibrary: PTRACE_CONT failed: " + lastErrorStr()};
    }

    // Wait for int3 trap.
    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        ptrace_ops::detach(pid);
        return {false, "injectLibrary: waitpid failed: " + lastErrorStr()};
    }

    if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP) {
        ptrace_ops::detach(pid);
        if (WIFEXITED(status)) {
            return {false, "injectLibrary: target exited during dlopen"};
        }
        return {false, "injectLibrary: unexpected stop (signal=" +
                       std::to_string(WSTOPSIG(status)) + ")"};
    }

    // Read RAX for dlopen return value.
    user_regs_struct afterRegs;
    if (!ptrace_ops::get_regs(pid, &afterRegs, sizeof(afterRegs))) {
        ptrace_ops::detach(pid);
        return {false, "injectLibrary: get_regs after dlopen failed"};
    }
    dlopenRet = afterRegs.rax;

    // Restore RIP.
    afterRegs.rip = savedRip;
    ptrace_ops::set_regs(pid, &afterRegs, sizeof(afterRegs));

    // 8. Free the path memory.
    uintptr_t munmapRet = 0;
    ptrace_ops::remote_syscall(pid, 11, remotePath, pathSize,
                                0, 0, 0, 0, munmapRet);

    // 9. Free the scratch page.
    ptrace_ops::remote_syscall(pid, 11, scratch, 0x1000,
                                0, 0, 0, 0, munmapRet);

    // 10. Detach.
    ptrace_ops::detach(pid);

    if (dlopenRet == 0) {
        return {false, "injectLibrary: dlopen returned NULL"};
    }

    return {true, ""};
}

// ---------------------------------------------------------------------------
// ejectLibrary (Linux ptrace version)
// ---------------------------------------------------------------------------

InjectResult ejectLibrary(int pid, const fs::path& lib_path) {
    // 1. Attach.
    if (!ptrace_ops::can_attach(pid)) {
        return {false, std::string{"ejectLibrary: cannot attach: "} +
                       ptrace_ops::last_error_str()};
    }
    if (!ptrace_ops::attach(pid)) {
        return {false, std::string{"ejectLibrary: attach failed: "} +
                       ptrace_ops::last_error_str()};
    }

    // 2. Find the loaded library base.
    std::string libName = lib_path.filename().string();
    uintptr_t libBase = ptrace_ops::find_library_base(pid, libName);
    if (libBase == 0) {
        ptrace_ops::detach(pid);
        return {false, "ejectLibrary: library \"" + libName +
                       "\" not found in target process"};
    }

    // 3. Find dlclose.
    uintptr_t dlcloseAddr = 0;
    // Try libdl first.
    uintptr_t dlBase = ptrace_ops::find_library_base(pid, "libdl.so.2");
    fs::path dlPath;
    if (dlBase != 0) {
        std::vector<ptrace_ops::MapEntry> maps;
        if (ptrace_ops::read_maps(pid, maps)) {
            for (const auto& m : maps) {
                if (m.path.find("libdl") != std::string::npos &&
                    m.start == dlBase) {
                    dlPath = m.path;
                    break;
                }
            }
        }
        if (!dlPath.empty()) {
            uintptr_t off = findElfExportOffset(dlPath, "dlclose");
            if (off != 0) dlcloseAddr = dlBase + off;
        }
    }
    if (dlcloseAddr == 0) {
        ptrace_ops::detach(pid);
        return {false, "ejectLibrary: cannot find dlclose"};
    }

    // 4. Allocate scratch page and inject dlclose trampoline.
    uintptr_t scratch = 0;
    if (!ptrace_ops::remote_syscall(pid, 9, 0, 0x1000,
            PROT_READ | PROT_WRITE | PROT_EXEC,
            MAP_PRIVATE | MAP_ANONYMOUS,
            static_cast<uintptr_t>(-1), 0,
            scratch) || scratch == 0 ||
        scratch == static_cast<uintptr_t>(-1)) {
        ptrace_ops::detach(pid);
        return {false, "ejectLibrary: remote mmap failed"};
    }

    // Write trampoline: movabs rdi, libBase; movabs rax, dlclose; call rax; int3.
    std::vector<uint8_t> trampoline;
    trampoline.push_back(0x48); trampoline.push_back(0xBF);
    for (int i = 0; i < 8; ++i)
        trampoline.push_back(static_cast<uint8_t>((libBase >> (i * 8)) & 0xFF));
    trampoline.push_back(0x48); trampoline.push_back(0xB8);
    for (int i = 0; i < 8; ++i)
        trampoline.push_back(static_cast<uint8_t>((dlcloseAddr >> (i * 8)) & 0xFF));
    trampoline.push_back(0xFF); trampoline.push_back(0xD0); // call rax
    trampoline.push_back(0xCC); // int3
    ptrace_ops::write_mem(pid, scratch, trampoline.data(), trampoline.size());

    // Save regs, set RIP=scratch, CONT, wait for SIGTRAP, restore.
    user_regs_struct savedRegs;
    ptrace_ops::get_regs(pid, &savedRegs, sizeof(savedRegs));
    user_regs_struct regs = savedRegs;
    regs.rip = scratch;
    ptrace_ops::set_regs(pid, &regs, sizeof(regs));

    ptrace(PTRACE_CONT, pid, nullptr, nullptr);
    int status = 0;
    waitpid(pid, &status, 0);

    // Restore.
    ptrace_ops::set_regs(pid, &savedRegs, sizeof(savedRegs));

    // Free scratch.
    uintptr_t munmapRet = 0;
    ptrace_ops::remote_syscall(pid, 11, scratch, 0x1000,
                                0, 0, 0, 0, munmapRet);

    ptrace_ops::detach(pid);

    return {true, ""};
}

// ---------------------------------------------------------------------------
// resolveDependencyClosure (ELF version)
// ---------------------------------------------------------------------------

std::vector<fs::path> resolveDependencyClosure(
    const fs::path& dllPath,
    const std::vector<fs::path>& searchDirs)
{
    return resolveElfDependencyClosure(dllPath, searchDirs);
}

// ---------------------------------------------------------------------------
// performInitHandshake (Linux ptrace version)
// ---------------------------------------------------------------------------

uint16_t performInitHandshake(int pid, const fs::path& lib_path,
                               const std::string& workspace_path,
                               const std::string& session_id,
                               const std::string& token,
                               const fs::path& port_file_path) {
    // 1. Read the .so from disk and find qt_commander_init offset.
    uintptr_t initOffset = findElfExportOffset(lib_path, "qt_commander_init");
    if (initOffset == 0) return 0;

    // 2. Attach to the target.
    if (!ptrace_ops::can_attach(pid)) return 0;
    if (!ptrace_ops::attach(pid)) return 0;

    // 3. Find the loaded library base.
    std::string libName = lib_path.filename().string();
    uintptr_t libBase = ptrace_ops::find_library_base(pid, libName);
    if (libBase == 0) {
        ptrace_ops::detach(pid);
        return 0;
    }

    // 4. Compute init function address in target.
    uintptr_t initAddr = libBase + initOffset;

    // 5. Fill InitParams.
#pragma pack(push, 1)
    struct InitParams {
        uint32_t version = 1;
        uint32_t total_size = 1024;
        char workspace_path[256] = {};
        char session_id[13] = {};
        char token[65] = {};
        char port_file_path[256] = {};
        uint8_t reserved[426] = {};
    } params;
#pragma pack(pop)

    auto safeCopy = [](char* dst, size_t dstLen, const std::string& src) {
        size_t n = std::min(src.size(), dstLen - 1);
        std::memcpy(dst, src.data(), n);
        dst[n] = '\0';
    };
    safeCopy(params.workspace_path, sizeof(params.workspace_path), workspace_path);
    safeCopy(params.session_id, sizeof(params.session_id), session_id);
    safeCopy(params.token, sizeof(params.token), token);
    safeCopy(params.port_file_path, sizeof(params.port_file_path), port_file_path.string());

    // 6. Allocate memory for InitParams in target.
    size_t paramSize = (sizeof(InitParams) + 0xFFF) & ~static_cast<size_t>(0xFFF);
    uintptr_t remoteParams = 0;
    if (!ptrace_ops::remote_syscall(pid, 9, 0, paramSize,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            static_cast<uintptr_t>(-1), 0,
            remoteParams) || remoteParams == 0 ||
        remoteParams == static_cast<uintptr_t>(-1)) {
        ptrace_ops::detach(pid);
        return 0;
    }

    // 7. Write InitParams.
    if (!ptrace_ops::write_mem(pid, remoteParams, &params, sizeof(InitParams))) {
        ptrace_ops::remote_syscall(pid, 11, remoteParams, paramSize,
                                    0, 0, 0, 0, remoteParams);
        ptrace_ops::detach(pid);
        return 0;
    }

    // 8. Allocate scratch page and inject init trampoline.
    uintptr_t scratch = 0;
    if (!ptrace_ops::remote_syscall(pid, 9, 0, 0x1000,
            PROT_READ | PROT_WRITE | PROT_EXEC,
            MAP_PRIVATE | MAP_ANONYMOUS,
            static_cast<uintptr_t>(-1), 0,
            scratch) || scratch == 0 ||
        scratch == static_cast<uintptr_t>(-1)) {
        ptrace_ops::remote_syscall(pid, 11, remoteParams, paramSize,
                                    0, 0, 0, 0, remoteParams);
        ptrace_ops::detach(pid);
        return 0;
    }

    // Trampoline: movabs rdi, remoteParams; movabs rax, initAddr; call rax; int3
    std::vector<uint8_t> trampoline;
    trampoline.push_back(0x48); trampoline.push_back(0xBF);
    for (int i = 0; i < 8; ++i)
        trampoline.push_back(static_cast<uint8_t>((remoteParams >> (i * 8)) & 0xFF));
    trampoline.push_back(0x48); trampoline.push_back(0xB8);
    for (int i = 0; i < 8; ++i)
        trampoline.push_back(static_cast<uint8_t>((initAddr >> (i * 8)) & 0xFF));
    trampoline.push_back(0xFF); trampoline.push_back(0xD0);
    trampoline.push_back(0xCC);
    ptrace_ops::write_mem(pid, scratch, trampoline.data(), trampoline.size());

    // Save + set RIP, CONT, wait for SIGTRAP.
    user_regs_struct savedRegs;
    if (!ptrace_ops::get_regs(pid, &savedRegs, sizeof(savedRegs))) {
        ptrace_ops::detach(pid);
        return 0;
    }
    user_regs_struct regs = savedRegs;
    regs.rip = scratch;
    ptrace_ops::set_regs(pid, &regs, sizeof(regs));

    ptrace(PTRACE_CONT, pid, nullptr, nullptr);
    int status = 0;
    waitpid(pid, &status, 0);

    // Restore regs.
    ptrace_ops::set_regs(pid, &savedRegs, sizeof(savedRegs));

    // Free scratch + params.
    uintptr_t dmy = 0;
    ptrace_ops::remote_syscall(pid, 11, scratch, 0x1000,
                                0, 0, 0, 0, dmy);
    ptrace_ops::remote_syscall(pid, 11, remoteParams, paramSize,
                                0, 0, 0, 0, dmy);

    // 9. Detach.
    ptrace_ops::detach(pid);

    // 10. Poll port file.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int delayMs = 50;
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            delayMs = std::min(delayMs * 2, 3200);
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

// ---------------------------------------------------------------------------
// isQtProcess (Linux version)
// ---------------------------------------------------------------------------

bool isQtProcess(int pid) {
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    if (!maps.is_open()) return false;
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find("libQt5Core.so") != std::string::npos ||
            line.find("libQt6Core.so") != std::string::npos)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// generateToken (Linux version)
// ---------------------------------------------------------------------------

std::string generateToken() {
    std::ifstream urand("/dev/urandom", std::ios::binary);
    if (!urand.is_open())
        throw std::runtime_error("cannot open /dev/urandom");
    uint8_t bytes[32];
    urand.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
    if (!urand) {
        throw std::runtime_error("read /dev/urandom failed");
    }
    static const char hex[] = "0123456789abcdef";
    std::string token;
    token.reserve(64);
    for (int i = 0; i < 32; ++i) {
        token += hex[bytes[i] >> 4];
        token += hex[bytes[i] & 0x0F];
    }
    // Zero the raw bytes.
    std::memset(bytes, 0, sizeof(bytes));
    return token;
}

#endif // __linux__
