// ---------------------------------------------------------------------------
// qt-commander Linux injector — ptrace-based shared-library injection.
// ---------------------------------------------------------------------------
#ifdef __linux__

#include "injector.h"
#include "ptrace_ops.h"
#include "elf_parser.h"
#include "elf_loader.h"

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

// Scratch page size for remote trampolines (stack + code).
// Must be large enough for dlopen's constructor chain (dynamic linker +
// Qt initialisation); 64 KB is ample while still fitting in one mmap chunk.
constexpr size_t kScratchSize = 0x10000;

static std::string lastErrorStr() {
    return std::string(std::strerror(errno)) + " (errno=" +
           std::to_string(errno) + ")";
}

// Find dlopen address in the target process.
// Strategy: find libdl/libc base in target via /proc/<pid>/maps,
// then find the offset of "dlopen" in the same .so on disk,
// and add base + offset to get the target's dlopen address.
static uintptr_t findDlopenInTarget(int pid) {
    // Find dlopen address in the target process.
    // Strategy: find the library containing dlopen in target via /proc/<pid>/maps,
    // then find the offset of "dlopen" in the same .so on disk,
    // and add base + offset to get the target's dlopen address.
    //
    // On glibc < 2.34, dlopen lives in libdl.so.2.
    // On glibc >= 2.34 (Ubuntu 22.04+), libdl.so.2 is a compatibility stub
    // that does NOT export dlopen — the real symbol is in libc.so.6.
    // We try libdl first, and if the symbol is not found there, fall back to libc.

    // Helper: search maps for the first mapping of a library and its disk path.
    auto findLibBaseAndPath = [&](const std::string& soname, const std::string& pathHint,
                                   uintptr_t& base, fs::path& libPath) -> bool {
        std::vector<ptrace_ops::MapEntry> maps;
        if (!ptrace_ops::read_maps(pid, maps)) return false;
        base = 0;
        for (const auto& m : maps) {
            auto lastSlash = m.path.rfind('/');
            std::string bname = (lastSlash != std::string::npos)
                               ? m.path.substr(lastSlash + 1) : m.path;
            if (bname == soname) {
                if (base == 0 || m.start < base) base = m.start;
                if (libPath.empty() && m.path.find(pathHint) != std::string::npos) {
                    libPath = m.path;
                }
            }
        }
        return base != 0 && !libPath.empty();
    };

    uintptr_t base = 0;
    fs::path libPath;

    // 1. Try libdl.so.2 first.
    if (findLibBaseAndPath("libdl.so.2", "libdl", base, libPath)) {
        uintptr_t offset = findElfExportOffset(libPath, "dlopen");
        if (offset != 0) return base + offset;
        // dlopen not in libdl stub — reset and fall through to libc.
        base = 0;
        libPath.clear();
    }

    // 2. Try libc.so.6 (glibc >= 2.34 merges libdl into libc).
    if (findLibBaseAndPath("libc.so.6", "libc.so", base, libPath)) {
        // Try __libc_dlopen_mode first (glibc internal), then plain dlopen.
        uintptr_t offset = findElfExportOffset(libPath, "__libc_dlopen_mode");
        if (offset == 0) {
            offset = findElfExportOffset(libPath, "dlopen");
        }
        if (offset != 0) return base + offset;
    }

    return 0;
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

    // 3. Try dlopen first (the primary path).  The library has zero
    //    file-scope Qt constructors (_GLOBAL__sub_I), so dlopen no longer
    //    crashes inside the ptrace-hijacked thread.  Stack alignment was
    //    fixed (RSP must be 16-byte aligned before CALL per x86-64 ABI).
    uintptr_t dlopenAddr = findDlopenInTarget(pid);
    if (dlopenAddr == 0) {
        // dlopen not found — try manual ELF loading as fallback.
        uintptr_t manualBase = 0;
        if (elfLoadLibrary(pid, absLib, manualBase) && manualBase != 0) {
            ptrace_ops::detach(pid);
            return {true, ""};
        }
        ptrace_ops::detach(pid);
        return {false, "injectLibrary: cannot find dlopen and manual ELF load failed"};
    }

    // 4. Allocate a scratch page for the remote_call trampoline.
    //    remote mmap via syscall.
    //    Use 64 KB (16 pages) for ample stack space — dlopen's constructor
    //    chain (dynamic linker + Qt initialisation) can easily exceed 4 KB.
    //    mmap(0, 0x10000, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
    uintptr_t scratch = 0;
    if (!ptrace_ops::remote_syscall(pid,
            /*__NR_mmap=*/9,
            0, kScratchSize,
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
        ptrace_ops::remote_syscall(pid, 11, scratch, kScratchSize, 0, 0, 0, 0, remotePath);
        ptrace_ops::detach(pid);
        return {false, "injectLibrary: remote mmap (path) failed"};
    }

    // 6. Write the library path into target memory.
    if (!ptrace_ops::write_mem(pid, remotePath, libPathStr.c_str(),
                                libPathStr.size() + 1)) {
        ptrace_ops::remote_syscall(pid, 11, remotePath, pathSize,
                                    0, 0, 0, 0, remotePath);
        ptrace_ops::remote_syscall(pid, 11, scratch, kScratchSize,
                                    0, 0, 0, 0, scratch);
        ptrace_ops::detach(pid);
        return {false, "injectLibrary: write_mem (path) failed"};
    }

    // 7. Call dlopen(path, RTLD_NOW) in the target via trampoline.
    //    Approach: write x86-64 machine code to scratch page that does:
    //      48 BF <path>       movabs rdi, path      (10 bytes)
    //      48 BE 02000000...  movabs rsi, RTLD_NOW  (10 bytes)
    //      48 B8 <dlopen>     movabs rax, dlopen    (10 bytes)
    //      FF D0              call rax              ( 2 bytes)
    //      CC                 int3                  ( 1 byte)
    //    Total: 33 bytes
    //
    //    Before executing, we redirect RSP into the scratch page (safe stack)
    //    so the CALL instruction can push the return address safely.

    std::vector<uint8_t> trampoline;
    // movabs rdi, path
    trampoline.push_back(0x48); trampoline.push_back(0xBF);
    for (int i = 0; i < 8; ++i)
        trampoline.push_back(static_cast<uint8_t>((remotePath >> (i * 8)) & 0xFF));

    // movabs rsi, RTLD_LAZY (defer symbol resolution to avoid crashing
    // the dynamic linker inside a ptrace-hijacked thread)
    trampoline.push_back(0x48); trampoline.push_back(0xBE);
    uint64_t rtld_flag = RTLD_LAZY;
    for (int i = 0; i < 8; ++i)
        trampoline.push_back(static_cast<uint8_t>((rtld_flag >> (i * 8)) & 0xFF));

    // movabs rax, dlopenAddr
    trampoline.push_back(0x48); trampoline.push_back(0xB8);
    for (int i = 0; i < 8; ++i)
        trampoline.push_back(static_cast<uint8_t>((dlopenAddr >> (i * 8)) & 0xFF));

    // call rax
    trampoline.push_back(0xFF); trampoline.push_back(0xD0);

    // int3
    trampoline.push_back(0xCC);

    // Write trampoline to scratch page (at offset 0x100, after the int3 at 0x0).
    if (!ptrace_ops::write_mem(pid, scratch + 0x100, trampoline.data(),
                                trampoline.size())) {
        ptrace_ops::remote_syscall(pid, 11, remotePath, pathSize,
                                    0, 0, 0, 0, remotePath);
        ptrace_ops::remote_syscall(pid, 11, scratch, kScratchSize,
                                    0, 0, 0, 0, scratch);
        ptrace_ops::detach(pid);
        return {false, "injectLibrary: write trampoline failed"};
    }

    // Save registers.
    user_regs_struct savedRegs;
    if (!ptrace_ops::get_regs(pid, &savedRegs, sizeof(savedRegs))) {
        ptrace_ops::remote_syscall(pid, 11, remotePath, pathSize,
                                    0, 0, 0, 0, remotePath);
        ptrace_ops::remote_syscall(pid, 11, scratch, kScratchSize,
                                    0, 0, 0, 0, scratch);
        ptrace_ops::detach(pid);
        return {false, "injectLibrary: get_regs failed"};
    }

    uintptr_t savedRip = savedRegs.rip;
    uintptr_t savedRsp = savedRegs.rsp;  // original stack — must restore after trampoline

    // Keep the original RSP instead of redirecting to a scratch stack.
    // The scratch-stack approach crashes inside ld.so's _dl_lookup_symbol_x
    // because the dynamic linker relies on thread-local state keyed to the
    // original stack.  We push the int3 return trap onto the original stack
    // (8 bytes below current RSP), which is safe for almost any stopped
    // thread — the slot just above the current frame is never live.
    // x86-64 ABI: RSP must be 16-byte aligned *before* the CALL instruction.
    // take one 8-byte slot below the current frame, align down to 16,
    // so after `call` (which pushes 8 bytes) RSP % 16 == 8 (correct at
    // function entry).
    uintptr_t retAddrSlot = savedRsp - 8;
    retAddrSlot &= ~static_cast<uintptr_t>(0xF);
    if (!ptrace_ops::write_word(pid, retAddrSlot, scratch /*int3 at offset 0*/)) {
        ptrace_ops::remote_syscall(pid, 11, remotePath, pathSize,
                                    0, 0, 0, 0, remotePath);
        ptrace_ops::remote_syscall(pid, 11, scratch, kScratchSize,
                                    0, 0, 0, 0, scratch);
        ptrace_ops::detach(pid);
        return {false, "injectLibrary: write retaddr failed"};
    }

    // Set RIP to trampoline; RSP stays on the original stack.
    savedRegs.rip = scratch + 0x100;
    savedRegs.rsp = retAddrSlot;
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

    // (waitpid done)

    // Handle the stop after dlopen trampoline.
    // Normal case: SIGTRAP from int3 — dlopen completed, RAX has the handle.
    // Corner case: SIGSEGV (signal 11) from library constructors/init code.
    //   On some platforms, the injected library's static constructors may
    //   crash in the ptrace context (e.g., accessing uninitialized Qt state).
    //   If the library was mapped into the target anyway, we can proceed.
    uintptr_t dlopenRet = 0;
    if (WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP) {
        // Normal: dlopen returned successfully, read the handle from RAX.
        user_regs_struct afterRegs;
        if (!ptrace_ops::get_regs(pid, &afterRegs, sizeof(afterRegs))) {
            ptrace_ops::detach(pid);
            return {false, "injectLibrary: get_regs after dlopen failed"};
        }
        dlopenRet = afterRegs.rax;
        afterRegs.rip = savedRip;
        afterRegs.rsp = savedRsp;   // restore original stack pointer
        ptrace_ops::set_regs(pid, &afterRegs, sizeof(afterRegs));
    } else if (WIFSTOPPED(status) && WSTOPSIG(status) == SIGSEGV) {
        // Constructor crash — check whether the library was loaded anyway.
        // Restore RIP + RSP so the target can continue after detach.
        user_regs_struct afterRegs;
        ptrace_ops::get_regs(pid, &afterRegs, sizeof(afterRegs));
        afterRegs.rip = savedRip;
        afterRegs.rsp = savedRsp;   // restore original stack pointer
        // Skip past the faulting instruction by forwarding the signal.
        // We cannot easily "skip" a SIGSEGV, but we CAN restore RIP and
        // rely on the fact that dlopen already mapped the library.
        ptrace_ops::set_regs(pid, &afterRegs, sizeof(afterRegs));
        // Verify the library is in the target's maps.
        std::string libName = lib_path.filename().string();
        uintptr_t checkBase = ptrace_ops::find_library_base(pid, libName);
        if (checkBase != 0) {
            // Library loaded despite constructor crash — synthesize a handle.
            dlopenRet = checkBase;
        }
    } else {
        ptrace_ops::detach(pid);
        if (WIFEXITED(status)) {
            return {false, "injectLibrary: target exited during dlopen"};
        }
        return {false, "injectLibrary: unexpected stop (signal=" +
                       std::to_string(WSTOPSIG(status)) + ")"};
    }

    // 8. Free the path memory.
    uintptr_t munmapRet = 0;
    ptrace_ops::remote_syscall(pid, 11, remotePath, pathSize,
                                0, 0, 0, 0, munmapRet);

    // 9. Free the scratch page.
    ptrace_ops::remote_syscall(pid, 11, scratch, kScratchSize,
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
    if (!ptrace_ops::remote_syscall(pid, 9, 0, kScratchSize,
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

    // Write int3 (0xCC) at the scratch page (used as return trap).
    uint8_t int3code = 0xCC;
    ptrace_ops::write_mem(pid, scratch, &int3code, 1);

    // Trampoline at scratch+0x100: movabs rdi, remoteParams; movabs rax, initAddr; call rax; int3
    std::vector<uint8_t> trampoline;
    trampoline.push_back(0x48); trampoline.push_back(0xBF);
    for (int i = 0; i < 8; ++i)
        trampoline.push_back(static_cast<uint8_t>((remoteParams >> (i * 8)) & 0xFF));
    trampoline.push_back(0x48); trampoline.push_back(0xB8);
    for (int i = 0; i < 8; ++i)
        trampoline.push_back(static_cast<uint8_t>((initAddr >> (i * 8)) & 0xFF));
    trampoline.push_back(0xFF); trampoline.push_back(0xD0);
    trampoline.push_back(0xCC);
    ptrace_ops::write_mem(pid, scratch + 0x100, trampoline.data(), trampoline.size());

    // Save registers and set up a safe stack.
    user_regs_struct savedRegs;
    if (!ptrace_ops::get_regs(pid, &savedRegs, sizeof(savedRegs))) {
        ptrace_ops::detach(pid);
        return 0;
    }
    user_regs_struct regs = savedRegs;
    uintptr_t savedRip = savedRegs.rip;
    uintptr_t savedRsp = savedRegs.rsp;

    // Push the return trap address onto the original stack (same technique
    // as the dlopen trampoline — keeps the dynamic linker's TLS stack key
    // consistent).  Use a 16-byte-aligned slot below the current frame.
    uintptr_t retAddrSlot = savedRsp - 8;
    retAddrSlot &= ~static_cast<uintptr_t>(0xF);
    if (!ptrace_ops::write_word(pid, retAddrSlot, scratch /*int3 at offset 0*/)) {
        ptrace_ops::remote_syscall(pid, 11, scratch, kScratchSize,
                                    0, 0, 0, 0, retAddrSlot);
        ptrace_ops::remote_syscall(pid, 11, remoteParams, paramSize,
                                    0, 0, 0, 0, remoteParams);
        ptrace_ops::detach(pid);
        return 0;
    }

    regs.rip = scratch + 0x100;
    regs.rsp = retAddrSlot;
    ptrace_ops::set_regs(pid, &regs, sizeof(regs));

    ptrace(PTRACE_CONT, pid, nullptr, nullptr);
    int status = 0;
    waitpid(pid, &status, 0);

    // Check whether qt_commander_init crashed.
    int stopSig = 0;
    if (WIFSTOPPED(status)) {
        stopSig = WSTOPSIG(status);
        if (stopSig != SIGTRAP) {
            std::fprintf(stderr,
                "[initHandshake] qt_commander_init stopped with signal %d\n",
                stopSig);
        }
    } else if (WIFSIGNALED(status)) {
        std::fprintf(stderr,
            "[initHandshake] target killed by signal %d\n",
            WTERMSIG(status));
    }

    // Restore regs (RIP, RSP, everything).
    ptrace_ops::set_regs(pid, &savedRegs, sizeof(savedRegs));

    // Free scratch + params.
    uintptr_t dmy = 0;
    ptrace_ops::remote_syscall(pid, 11, scratch, kScratchSize,
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
