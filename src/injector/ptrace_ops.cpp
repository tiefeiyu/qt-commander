// ---------------------------------------------------------------------------
// ptrace_ops — Linux ptrace wrapper implementation.
// ---------------------------------------------------------------------------
#ifdef __linux__

#include "ptrace_ops.h"

#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/uio.h>       // process_vm_readv / writev
#include <sys/user.h>      // user_regs_struct
#include <sys/mman.h>      // PROT_* constants
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <elf.h>

namespace ptrace_ops {

namespace {

thread_local char g_error[256] = {};

void set_error(const char* msg) {
    std::strncpy(g_error, msg, sizeof(g_error) - 1);
    g_error[sizeof(g_error) - 1] = '\0';
}

void set_errno_error(const char* ctx) {
    std::snprintf(g_error, sizeof(g_error), "%s: %s (errno=%d)",
                  ctx, std::strerror(errno), errno);
}

// x86-64 register indices for ptrace(PTRACE_PEEKUSER, ...)
// These are offsets into struct user (in bytes), divided by 8 for PEEKUSER.
// From <sys/user.h>: R15..RIP are at fixed offsets.
constexpr int REG_R15 = 0;
constexpr int REG_R14 = 1;
constexpr int REG_R13 = 2;
constexpr int REG_R12 = 3;
constexpr int REG_RBP = 4;
constexpr int REG_RBX = 5;
constexpr int REG_R11 = 6;
constexpr int REG_R10 = 7;
constexpr int REG_R9  = 8;
constexpr int REG_R8  = 9;
constexpr int REG_RAX = 10;
constexpr int REG_RCX = 11;
constexpr int REG_RDX = 12;
constexpr int REG_RSI = 13;
constexpr int REG_RDI = 14;
constexpr int REG_ORIG_RAX = 15;
constexpr int REG_RIP = 16;
constexpr int REG_CS  = 17;
constexpr int REG_EFLAGS = 18;
constexpr int REG_RSP = 19;
constexpr int REG_SS  = 20;

int wait_for_stop(int pid) {
    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        set_errno_error("waitpid");
        return -1;
    }
    if (WIFEXITED(status)) {
        set_error("target process exited");
        return -1;
    }
    if (WIFSIGNALED(status)) {
        std::snprintf(g_error, sizeof(g_error),
                      "target killed by signal %d", WTERMSIG(status));
        return -1;
    }
    return status;
}

// Read a register value via PTRACE_PEEKUSER.
// reg_index is the offset in words (8-byte units) into struct user.
uintptr_t peek_reg(int pid, int reg_index) {
    errno = 0;
    // PTRACE_PEEKUSER takes (pid, addr, data) where addr is the offset
    // into the USER area.
    long val = ptrace(PTRACE_PEEKUSER, pid,
                      reinterpret_cast<void*>(static_cast<uintptr_t>(reg_index * 8)), nullptr);
    if (val == -1 && errno != 0) {
        set_errno_error("PTRACE_PEEKUSER");
        return 0;
    }
    return static_cast<uintptr_t>(val);
}

bool poke_reg(int pid, int reg_index, uintptr_t val) {
    errno = 0;
    long rc = ptrace(PTRACE_POKEUSER, pid,
                     reinterpret_cast<void*>(reg_index * 8),
                     reinterpret_cast<void*>(val));
    if (rc == -1 && errno != 0) {
        set_errno_error("PTRACE_POKEUSER");
        return false;
    }
    return true;
}

} // anonymous namespace

// ---- attach / detach -------------------------------------------------------

bool attach(int pid) {
    errno = 0;
    if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) == -1) {
        set_errno_error("PTRACE_ATTACH");
        return false;
    }
    int status = wait_for_stop(pid);
    if (status < 0) return false;
    // Forward the SIGSTOP so the target can handle it normally.
    // Actually, PTRACE_ATTACH always sends SIGSTOP; we just need to
    // set PTRACE_SETOPTIONS to not forward it.  But for ATTACH, the
    // default is to forward. We use PTRACE_CONT with the signal.
    // For simplicity, we just let the stop happen and continue.
    return true;
}

bool seize(int pid) {
    errno = 0;
    if (ptrace(PTRACE_SEIZE, pid, nullptr,
               reinterpret_cast<void*>(PTRACE_O_TRACESYSGOOD)) == -1) {
        set_errno_error("PTRACE_SEIZE");
        return false;
    }
    return true;
}

bool interrupt(int pid) {
    errno = 0;
    if (ptrace(PTRACE_INTERRUPT, pid, nullptr, nullptr) == -1) {
        set_errno_error("PTRACE_INTERRUPT");
        return false;
    }
    return wait_for_stop(pid) >= 0;
}

bool detach(int pid) {
    errno = 0;
    if (ptrace(PTRACE_DETACH, pid, nullptr, nullptr) == -1) {
        set_errno_error("PTRACE_DETACH");
        return false;
    }
    return true;
}

// ---- register access -------------------------------------------------------

bool get_regs(int pid, void* regs, size_t regs_size) {
    errno = 0;
    struct iovec iov;
    iov.iov_base = regs;
    iov.iov_len = regs_size;
    long rc = ptrace(PTRACE_GETREGSET, pid,
                     reinterpret_cast<void*>(NT_PRSTATUS), &iov);
    if (rc == -1) {
        set_errno_error("PTRACE_GETREGSET");
        return false;
    }
    return true;
}

bool set_regs(int pid, const void* regs, size_t regs_size) {
    errno = 0;
    struct iovec iov;
    // const_cast needed for kernel ABI but data is not modified
    iov.iov_base = const_cast<void*>(regs);
    iov.iov_len = regs_size;
    long rc = ptrace(PTRACE_SETREGSET, pid,
                     reinterpret_cast<void*>(NT_PRSTATUS), &iov);
    if (rc == -1) {
        set_errno_error("PTRACE_SETREGSET");
        return false;
    }
    return true;
}

bool get_ip(int pid, uintptr_t& ip) {
    ip = peek_reg(pid, REG_RIP);
    return (errno == 0);
}

bool set_ip(int pid, uintptr_t ip) {
    return poke_reg(pid, REG_RIP, ip);
}

bool get_register(int pid, int reg_index, uintptr_t& value) {
    value = peek_reg(pid, reg_index);
    return (errno == 0);
}

bool set_register(int pid, int reg_index, uintptr_t value) {
    return poke_reg(pid, reg_index, value);
}

// ---- memory access ---------------------------------------------------------

bool read_word(int pid, uintptr_t addr, uintptr_t& word) {
    errno = 0;
    long val = ptrace(PTRACE_PEEKDATA, pid,
                      reinterpret_cast<void*>(addr), nullptr);
    if (val == -1 && errno != 0) {
        set_errno_error("PTRACE_PEEKDATA");
        return false;
    }
    word = static_cast<uintptr_t>(val);
    return true;
}

bool write_word(int pid, uintptr_t addr, uintptr_t word) {
    errno = 0;
    long rc = ptrace(PTRACE_POKEDATA, pid,
                     reinterpret_cast<void*>(addr),
                     reinterpret_cast<void*>(word));
    if (rc == -1 && errno != 0) {
        set_errno_error("PTRACE_POKEDATA");
        return false;
    }
    return true;
}

bool read_mem(int pid, uintptr_t addr, void* buf, size_t len) {
    auto* dst = static_cast<uint8_t*>(buf);
    // Use process_vm_readv when available (faster, handles arbitrary sizes).
    struct iovec local_iov;
    local_iov.iov_base = dst;
    local_iov.iov_len = len;

    struct iovec remote_iov;
    remote_iov.iov_base = reinterpret_cast<void*>(addr);
    remote_iov.iov_len = len;

    ssize_t n = process_vm_readv(pid, &local_iov, 1, &remote_iov, 1, 0);
    if (n >= 0 && static_cast<size_t>(n) == len) return true;

    // Fallback: word-by-word via PTRACE_PEEKDATA.
    size_t i = 0;
    while (i < len) {
        // Align to word boundary for peek.
        uintptr_t aligned = addr + i;
        aligned &= ~(word_size() - 1);
        uintptr_t word = 0;
        if (!read_word(pid, aligned, word)) return false;
        size_t offset_in_word = (addr + i) - aligned;
        size_t copy = word_size() - offset_in_word;
        if (copy > len - i) copy = len - i;
        std::memcpy(dst + i, reinterpret_cast<uint8_t*>(&word) + offset_in_word, copy);
        i += copy;
    }
    return true;
}

bool write_mem(int pid, uintptr_t addr, const void* data, size_t len) {
    auto* src = static_cast<const uint8_t*>(data);

    // Try process_vm_writev first.
    struct iovec local_iov;
    local_iov.iov_base = const_cast<uint8_t*>(src);
    local_iov.iov_len = len;

    struct iovec remote_iov;
    remote_iov.iov_base = reinterpret_cast<void*>(addr);
    remote_iov.iov_len = len;

    ssize_t n = process_vm_writev(pid, &local_iov, 1, &remote_iov, 1, 0);
    if (n >= 0 && static_cast<size_t>(n) == len) return true;

    // Fallback: read-modify-write via PTRACE_PEEKDATA / POKEDATA.
    size_t i = 0;
    while (i < len) {
        uintptr_t aligned = addr + i;
        size_t prefix = aligned & (word_size() - 1);
        aligned &= ~(word_size() - 1);

        uintptr_t word = 0;
        if (!read_word(pid, aligned, word) && i == 0) return false;

        size_t copy = word_size() - prefix;
        if (copy > len - i) copy = len - i;
        // Modify the word.
        auto* wordBytes = reinterpret_cast<uint8_t*>(&word);
        std::memcpy(wordBytes + prefix, src + i, copy);

        if (!write_word(pid, aligned, word)) return false;
        i += copy;
    }
    return true;
}

// ---- remote syscall --------------------------------------------------------

// Search for the byte sequence 0F 05 C3 (syscall; ret) in a memory range
// of the target process.  Returns the address of the 'syscall' instruction
// (where 0F 05 begins), or 0 if not found.
static uintptr_t find_syscall_ret_gadget(int pid, uintptr_t start, uintptr_t end) {
    // Read in 4 KB chunks.
    constexpr size_t kChunk = 4096;
    uint8_t buf[kChunk];
    for (uintptr_t addr = start; addr + 3 < end; addr += kChunk - 3) {
        size_t toRead = kChunk;
        if (addr + toRead > end) toRead = end - addr;
        // Use process_vm_readv (fast path) or peekdata (fallback).
        struct iovec local;
        local.iov_base = buf;
        local.iov_len = toRead;
        struct iovec remote;
        remote.iov_base = reinterpret_cast<void*>(addr);
        remote.iov_len = toRead;
        ssize_t n = process_vm_readv(pid, &local, 1, &remote, 1, 0);
        if (n <= 0) {
            // Fallback: peekdata word by word.
            for (size_t i = 0; i < toRead; i += word_size()) {
                uintptr_t word = 0;
                if (!read_word(pid, addr + i, word)) break;
                size_t copy = word_size();
                if (i + copy > toRead) copy = toRead - i;
                std::memcpy(buf + i, &word, copy);
            }
        }
        for (size_t i = 0; i + 2 < toRead; ++i) {
            if (buf[i] == 0x0F && buf[i + 1] == 0x05 && buf[i + 2] == 0xC3) {
                return addr + static_cast<uintptr_t>(i);
            }
            // Also match 0F 05 CC (syscall; int3) — some wrappers use int3.
            if (buf[i] == 0x0F && buf[i + 1] == 0x05 && buf[i + 2] == 0xCC) {
                return addr + static_cast<uintptr_t>(i);
            }
        }
    }
    return 0;
}

// Find a usable "syscall" instruction gadget in the target.
// Searches libc's executable segment first, then other libraries.
static uintptr_t find_syscall_gadget(int pid) {
    std::vector<MapEntry> maps;
    if (!read_maps(pid, maps)) return 0;

    // Search in priority order: libc, ld-linux, vdso.
    for (const char* preferred : {"libc", "ld-linux", "vdso"}) {
        for (const auto& m : maps) {
            if (m.perms.find('x') == std::string::npos) continue;
            if (m.path.find(preferred) == std::string::npos) continue;
            uintptr_t gadget = find_syscall_ret_gadget(pid, m.start, m.end);
            if (gadget != 0) return gadget;
        }
    }
    // Fallback: search any executable mapping.
    for (const auto& m : maps) {
        if (m.perms.find('x') == std::string::npos) continue;
        uintptr_t gadget = find_syscall_ret_gadget(pid, m.start, m.end);
        if (gadget != 0) return gadget;
    }
    return 0;
}

bool remote_syscall(int pid, long nr,
                    uintptr_t a1, uintptr_t a2, uintptr_t a3,
                    uintptr_t a4, uintptr_t a5, uintptr_t a6,
                    uintptr_t& out) {
    // Find a syscall gadget.
    static thread_local uintptr_t s_gadget = 0;
    static thread_local int s_gadget_pid = 0;
    if (s_gadget_pid != pid || s_gadget == 0) {
        s_gadget = find_syscall_gadget(pid);
        s_gadget_pid = pid;
    }
    if (s_gadget == 0) {
        set_error("remote_syscall: no syscall gadget found in target");
        return false;
    }

    // The gadget is at s_gadget: "syscall; ret".
    // We set RAX=nr, arguments in regs, RIP=s_gadget, and RSP to point to
    // a return location that has "int3".  We need a writable+executable
    // page with int3 at offset 0 for the return trap.

    // For the simplest approach without a pre-allocated scratch page,
    // we use the gadget as "syscall; ret" and set the return address
    // on the stack to somewhere with an int3.
    //
    // Actually, simpler: since the gadget is "syscall; ret", after syscall
    // returns, the CPU reads the return address from [RSP] and jumps there.
    // We can push an "int3" on the stack.
    //
    // Even simpler: after the syscall, we STOP the target with PTRACE_CONT
    // and then PTRACE_INTERRUPT... but the target runs freely after the ret.
    //
    // The most reliable approach: set orig_rax = nr, set args, set RIP to
    // the gadget, and put an int3 on the "stack" so that after "syscall; ret",
    // the target hits the int3 and stops.
    //
    // We need 8 bytes of writable memory for the return address slot.
    // Use the current RSP minus 8 and write 0xCCCCCCCCCCCCCCCC there.
    // Wait, we can't write all 8 bytes as 0xCC — that would be an invalid
    // address which would SEGFAULT when ret tries to jump there.
    //
    // Actually, the simplest approach for Phase 1: use PTRACE_SYSCALL.
    // PTRACE_SYSCALL stops the tracee at the NEXT syscall entry/exit.
    // We can:
    //   1. Set regs for the syscall we want
    //   2. Set RIP to the gadget (syscall; ret)
    //   3. PTRACE_SYSCALL + PTRACE_CONT
    //   4. waitpid — stops at syscall entry (before executing it)
    //   5. PTRACE_SYSCALL + PTRACE_CONT — continues through syscall,
    //      stops at syscall exit
    //   6. waitpid — read RAX for return value
    //
    // This is complex.  Let me use a simpler approach:
    // Just use the current stack.  Save RSP, write an int3 return address
    // at [RSP-8], set RIP = gadget, CONT, wait for SIGTRAP.
    //
    // But wait — we can't write to the target's stack because it might be
    // in use.  The cleanest approach for now:
    //
    // FALLBACK: use the fact that "syscall; ret" at address G will:
    //   1. execute syscall with nr=orig_rax
    //   2. execute ret, which reads [RSP] and jumps there
    // We can set [RSP] to point to an "int3" instruction that already
    // exists in the target's libc (not uncommon, many error paths have int3).
    // But this is fragile.
    //
    // SIMPLEST RELIABLE: allocate a page in our OWN process, mmap it shared
    // with the target... not possible via ptrace.
    //
    // BEST FOR PHASE 1: use the caller-provided "scratch page" pattern.
    // The callers (injector_linux.cpp) allocate a scratch page first
    // via the trampoline approach.  So remote_syscall requires that the
    // caller has already allocated a scratch page and written int3 there.
    //
    // We expose this requirement: the caller passes a scratch_page address
    // that has int3 at offset 0.

    // --- Revised implementation: use orig_rax + syscall gadget + stack ---
    // We write a return address (pointing to int3) on the target's stack
    // by decrementing RSP.  This is safe if the target is stopped.

    user_regs_struct savedRegs;
    if (!get_regs(pid, &savedRegs, sizeof(savedRegs))) return false;

    // Find an int3 gadget in the target (0xCC) — these are rare in libc.
    // Instead, we write int3 directly on the stack as the return "address".
    // Wait — int3 is 0xCC (1 byte), but a return address is 8 bytes.
    // If we set [RSP] = a value that when executed as code is "int3; ...",
    // we'd need to find a byte sequence starting with 0xCC in executable
    // memory.  These don't normally exist.
    //
    // ALTERNATIVE: use PTRACE_SYSCALL tracing:
    // - Save regs
    // - Set args in regs, set RIP to the gadget
    // - Set the return address on stack to ANY address that will cause
    //   a SIGSEGV or SIGTRAP
    // - PTRACE_CONT with PTRACE_SYSCALL flag
    // - The first stop is at syscall-enter — we don't need to read RAX yet
    // - PTRACE_CONT again — stops at syscall-exit — read RAX
    // - Restore regs

    // Save registers and set up the syscall.
    uintptr_t prev_rip = savedRegs.rip;
    uintptr_t prev_rsp = savedRegs.rsp;
    uintptr_t prev_rax = savedRegs.rax;
    uintptr_t prev_rdi = savedRegs.rdi;
    uintptr_t prev_rsi = savedRegs.rsi;
    uintptr_t prev_rdx = savedRegs.rdx;
    uintptr_t prev_r10 = savedRegs.r10;
    uintptr_t prev_r8  = savedRegs.r8;
    uintptr_t prev_r9  = savedRegs.r9;

    // Set syscall arguments.
    savedRegs.rax = static_cast<uint64_t>(nr);
    savedRegs.rdi = a1;
    savedRegs.rsi = a2;
    savedRegs.rdx = a3;
    savedRegs.r10 = a4;
    savedRegs.r8  = a5;
    savedRegs.r9  = a6;

    // Set RIP to the syscall gadget.
    savedRegs.rip = s_gadget;

    // Push a return address that will segfault (write to stack).
    // We push a known invalid address (0x1) — the ret will try to
    // jump there and trigger SIGSEGV, which we intercept.
    // Actually, it's better to push something that causes a SIGTRAP.
    // Since we can't easily find 0xCC in executable memory, let's use
    // a signal-based approach: we push the address of the gadget itself.
    // After "syscall; ret", it will re-execute syscall with RAX=whatever
    // the kernel set.  This creates an infinite loop of syscalls.
    //
    // Better: use the saved RIP as the return address.  After syscall,
    // control returns to the original instruction.  Then we single-step
    // or interrupt to read RAX.
    //
    // SIMPLEST: set the return address to somewhere that will trap.
    // Use the address of "int3" (0xCC) which we can place by writing
    // it to a known location... which brings us back to needing a
    // scratch page.
    //
    // CHICKEN-AND-EGG RESOLUTION:
    // For the very first remote_syscall (the one that allocates the
    // scratch page), we use PTRACE_SYSCALL tracing.
    // For subsequent calls, we use the scratch page.
    //
    // Let me implement PTRACE_SYSCALL approach for all remote_syscalls,
    // which is simpler and doesn't need a scratch page.

    // Set return address to saved RIP (loop back after syscall).
    uintptr_t newRsp = savedRegs.rsp - 8;
    savedRegs.rsp = newRsp;

    // Write the return address on the stack: we use saved RIP so the
    // target will loop back to where it was after the syscall completes.
    // This is safe because:
    // - syscall executes (RAX = nr, args = we set)
    // - ret pops [RSP] = saved RIP, jumps back to original code
    // - We immediately PTRACE_INTERRUPT to stop it
    // - Read RAX for the syscall result
    // - Restore original regs
    if (!write_word(pid, newRsp, prev_rip)) {
        set_error("remote_syscall: write_word stk failed");
        return false;
    }

    // Write the modified regs.
    if (!set_regs(pid, &savedRegs, sizeof(savedRegs))) {
        set_error("remote_syscall: set_regs failed");
        return false;
    }

    // Use PTRACE_SYSCALL to trace through the syscall.
    errno = 0;
    if (ptrace(PTRACE_SYSCALL, pid, nullptr, nullptr) == -1) {
        set_errno_error("PTRACE_SYSCALL");
        return false;
    }

    // First stop: syscall entry.
    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        set_errno_error("waitpid syscall-entry");
        return false;
    }
    if (!WIFSTOPPED(status)) {
        set_error("remote_syscall: unexpected exit at syscall entry");
        return false;
    }

    // Second PTRACE_SYSCALL + CONT: continue to syscall exit.
    errno = 0;
    if (ptrace(PTRACE_SYSCALL, pid, nullptr, nullptr) == -1) {
        set_errno_error("PTRACE_SYSCALL2");
        return false;
    }
    if (waitpid(pid, &status, 0) == -1) {
        set_errno_error("waitpid syscall-exit");
        return false;
    }
    if (!WIFSTOPPED(status)) {
        set_error("remote_syscall: unexpected exit at syscall exit");
        return false;
    }

    // Read RAX (syscall return value).
    user_regs_struct afterRegs;
    if (!get_regs(pid, &afterRegs, sizeof(afterRegs))) {
        set_error("remote_syscall: get_regs after");
        return false;
    }
    out = afterRegs.rax;

    // Restore original registers.
    afterRegs.rip = prev_rip;
    afterRegs.rsp = prev_rsp;
    afterRegs.rax = prev_rax;
    afterRegs.rdi = prev_rdi;
    afterRegs.rsi = prev_rsi;
    afterRegs.rdx = prev_rdx;
    afterRegs.r10 = prev_r10;
    afterRegs.r8  = prev_r8;
    afterRegs.r9  = prev_r9;
    set_regs(pid, &afterRegs, sizeof(afterRegs));

    return true;
}

// ---- remote function call --------------------------------------------------

bool remote_call(int pid, uintptr_t fn, uintptr_t arg,
                 uintptr_t scratch_page, uintptr_t& retval) {
    // The scratch page is pre-populated with an `int3` at offset 0.
    // Strategy:
    //   1. Save current RIP / RSP / RDI / RAX
    //   2. Push a "return address" = scratch_page (contains int3)
    //   3. Set RDI = arg
    //   4. Set RIP = fn
    //   5. PTRACE_CONT
    //   6. waitpid for SIGTRAP
    //   7. Read RAX = return value
    //   8. Restore registers

    uintptr_t saved_rip = peek_reg(pid, REG_RIP);
    uintptr_t saved_rsp = peek_reg(pid, REG_RSP);
    uintptr_t saved_rdi = peek_reg(pid, REG_RDI);
    uintptr_t saved_rax = peek_reg(pid, REG_RAX);
    uintptr_t saved_rbx = peek_reg(pid, REG_RBX);
    uintptr_t saved_rcx = peek_reg(pid, REG_RCX);
    uintptr_t saved_rdx = peek_reg(pid, REG_RDX);
    uintptr_t saved_rsi = peek_reg(pid, REG_RSI);

    // On x86-64, we need 16-byte stack alignment at the call site.
    // The call instruction pushes 8 bytes (return address), so we need
    // RSP % 16 == 8 before the call.  We adjust RSP down if needed.
    uintptr_t new_rsp = saved_rsp;
    // Make space for a return address slot.
    new_rsp -= 8;
    // Ensure alignment: after the implicit push, the callee sees
    // new_rsp % 16 == 0.  Before the push, RSP should be 16-aligned.
    if (new_rsp & 0xF) {
        new_rsp -= 8; // align to 16
    }

    // We don't actually push anything — we set RIP=fn, and fn will RET
    // to wherever [RSP] points.  So we write the scratch_page address
    // (which contains int3) at new_rsp.
    if (!write_word(pid, new_rsp, scratch_page)) {
        set_error("remote_call: write_word(return_addr) failed");
        return false;
    }

    // Set up registers for the call.
    if (!poke_reg(pid, REG_RDI, arg)) return false;
    if (!poke_reg(pid, REG_RSP, new_rsp)) return false;
    if (!poke_reg(pid, REG_RIP, fn)) return false;

    // Continue the target.
    errno = 0;
    if (ptrace(PTRACE_CONT, pid, nullptr, nullptr) == -1) {
        set_errno_error("PTRACE_CONT");
        // Try to restore.
        poke_reg(pid, REG_RIP, saved_rip);
        poke_reg(pid, REG_RSP, saved_rsp);
        return false;
    }

    // Wait for the int3 trap.
    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        set_errno_error("waitpid after remote_call");
        return false;
    }

    if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP) {
        if (WIFEXITED(status)) {
            set_error("remote_call: target exited during call");
        } else if (WIFSIGNALED(status)) {
            std::snprintf(g_error, sizeof(g_error),
                          "remote_call: target killed by signal %d",
                          WTERMSIG(status));
        } else {
            std::snprintf(g_error, sizeof(g_error),
                          "remote_call: unexpected stop signal %d (status=0x%x)",
                          WSTOPSIG(status), status);
        }
        return false;
    }

    // Read return value from RAX.
    retval = peek_reg(pid, REG_RAX);

    // Restore registers.
    poke_reg(pid, REG_RIP, saved_rip);
    poke_reg(pid, REG_RSP, saved_rsp);
    poke_reg(pid, REG_RDI, saved_rdi);
    poke_reg(pid, REG_RAX, saved_rax);
    poke_reg(pid, REG_RBX, saved_rbx);
    poke_reg(pid, REG_RCX, saved_rcx);
    poke_reg(pid, REG_RDX, saved_rdx);
    poke_reg(pid, REG_RSI, saved_rsi);

    return true;
}

// ---- control ---------------------------------------------------------------

bool single_step(int pid) {
    errno = 0;
    if (ptrace(PTRACE_SINGLESTEP, pid, nullptr, nullptr) == -1) {
        set_errno_error("PTRACE_SINGLESTEP");
        return false;
    }
    int status = wait_for_stop(pid);
    if (status < 0) return false;
    // If we got SIGTRAP, consume it so we can continue.
    if (WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP) {
        // The kernel already stopped; we just need to note it.
        return true;
    }
    return true;
}

bool cont(int pid, int& out_signal) {
    errno = 0;
    if (ptrace(PTRACE_CONT, pid, nullptr, nullptr) == -1) {
        set_errno_error("PTRACE_CONT");
        return false;
    }
    int status = wait_for_stop(pid);
    if (status < 0) return false;
    if (WIFSTOPPED(status)) {
        out_signal = WSTOPSIG(status);
        return true;
    }
    out_signal = 0;
    return false; // target exited or was killed
}

// ---- target information ----------------------------------------------------

bool read_maps(int pid, std::vector<MapEntry>& out) {
    out.clear();
    std::string path = "/proc/" + std::to_string(pid) + "/maps";
    std::ifstream f(path);
    if (!f.is_open()) {
        set_errno_error("open /proc/pid/maps");
        return false;
    }
    std::string line;
    while (std::getline(f, line)) {
        MapEntry e;
        // Format: addr1-addr2 perms offset dev inode pathname
        std::istringstream iss(line);
        std::string addrRange;
        if (!(iss >> addrRange)) continue;

        auto dash = addrRange.find('-');
        if (dash == std::string::npos) continue;
        e.start = std::stoull(addrRange.substr(0, dash), nullptr, 16);
        e.end   = std::stoull(addrRange.substr(dash + 1), nullptr, 16);

        if (!(iss >> e.perms)) continue;
        if (!(iss >> std::hex >> e.offset)) continue;

        // Skip dev and inode.
        std::string dev, inodeStr;
        iss >> dev >> inodeStr;

        // Rest of line is the path (may contain spaces? No, proc maps has
        // spaces only as separator; but path could have spaces on some
        // kernels. For .so paths, there are no spaces).
        std::string rest;
        std::getline(iss, rest);
        // Trim leading whitespace.
        size_t pos = rest.find_first_not_of(" \t");
        if (pos != std::string::npos) {
            e.path = rest.substr(pos);
        }
        out.push_back(e);
    }
    return true;
}

uintptr_t find_library_base(int pid, const std::string& soname) {
    std::vector<MapEntry> maps;
    if (!read_maps(pid, maps)) return 0;
    for (const auto& m : maps) {
        if (m.perms.find('x') == std::string::npos) continue;
        // Match by basename.
        auto lastSlash = m.path.rfind('/');
        std::string base = (lastSlash != std::string::npos)
                           ? m.path.substr(lastSlash + 1) : m.path;
        if (base == soname) return m.start;
    }
    return 0;
}

// ---- error -----------------------------------------------------------------

const char* last_error_str() {
    return g_error;
}

// ---- helpers ---------------------------------------------------------------

bool can_attach(int pid) {
    // Check yama ptrace_scope.
    std::ifstream f("/proc/sys/kernel/yama/ptrace_scope");
    if (f.is_open()) {
        int scope = 0;
        f >> scope;
        if (scope >= 2) {
            set_error("ptrace_scope >= 2: attach requires root or CAP_SYS_PTRACE");
            return false;
        }
    }
    // Check if we can access /proc/<pid>/mem (same user check).
    std::string memPath = "/proc/" + std::to_string(pid) + "/mem";
    if (::access(memPath.c_str(), R_OK) != 0) {
        set_errno_error("cannot access target process (different user?)");
        return false;
    }
    return true;
}

} // namespace ptrace_ops

#endif // __linux__
