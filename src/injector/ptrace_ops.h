#pragma once
// ---------------------------------------------------------------------------
// ptrace_ops — low-level Linux ptrace wrappers for remote-process manipulation.
// ---------------------------------------------------------------------------
#ifdef __linux__

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace ptrace_ops {

// ---- attach / detach -------------------------------------------------------
bool attach(int pid);
bool seize(int pid);
bool interrupt(int pid);
bool detach(int pid);

// ---- register access -------------------------------------------------------
bool get_regs(int pid, void* regs, size_t regs_size);
bool set_regs(int pid, const void* regs, size_t regs_size);
bool get_ip(int pid, uintptr_t& ip);
bool set_ip(int pid, uintptr_t ip);
bool get_register(int pid, int reg_index, uintptr_t& value);
bool set_register(int pid, int reg_index, uintptr_t value);

// ---- memory access ---------------------------------------------------------
bool read_word(int pid, uintptr_t addr, uintptr_t& word);
bool write_word(int pid, uintptr_t addr, uintptr_t word);
bool read_mem(int pid, uintptr_t addr, void* buf, size_t len);
bool write_mem(int pid, uintptr_t addr, const void* data, size_t len);

// ---- remote syscall --------------------------------------------------------
// Invoke a syscall in the target process.  Returns the result in `out`.
// The target is stopped after the call completes.
bool remote_syscall(int pid, long nr,
                    uintptr_t a1, uintptr_t a2, uintptr_t a3,
                    uintptr_t a4, uintptr_t a5, uintptr_t a6,
                    uintptr_t& out);

// ---- remote function call --------------------------------------------------
// Call fn(arg1, arg2) in the target via register-hijack + int3 return trap.
// arg1→RDI, arg2→RSI (x86-64 ABI).  Pass 0 for unused arg2.
// scratch_page: writable+executable page in target (allocated via remote mmap).
bool remote_call(int pid, uintptr_t fn, uintptr_t arg1, uintptr_t arg2,
                 uintptr_t scratch_page, uintptr_t& retval);

// ---- control ---------------------------------------------------------------
bool single_step(int pid);
bool cont(int pid, int& out_signal);

// ---- target information ----------------------------------------------------
struct MapEntry {
    uintptr_t start, end, offset;
    std::string perms, path;
};
bool read_maps(int pid, std::vector<MapEntry>& out);
uintptr_t find_library_base(int pid, const std::string& soname);

// ---- error -----------------------------------------------------------------
const char* last_error_str();

// ---- helpers ---------------------------------------------------------------
bool can_attach(int pid);
constexpr size_t word_size() { return sizeof(uintptr_t); }

} // namespace ptrace_ops

#endif // __linux__
