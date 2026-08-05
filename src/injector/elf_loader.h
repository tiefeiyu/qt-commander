#pragma once
// ---------------------------------------------------------------------------
// elf_loader — manual ELF shared-library loader for ptrace-based injection.
//
// glibc's ld.so crashes inside _dl_lookup_symbol_x when called from a
// ptrace-hijacked thread (the dynamic linker's per-thread state is
// inconsistent).  This module bypasses ld.so entirely:
//   1. mmap LOAD segments into the target via remote_syscall,
//   2. apply R_X86_64_RELATIVE and R_X86_64_GLOB_DAT relocations,
//   3. resolve external symbols against the target's already-loaded libs.
// ---------------------------------------------------------------------------
#ifdef __linux__

#include <cstdint>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

/// Load an ELF shared library into the target process without using dlopen.
/// Returns the library's base address stored in `outBase` on success.
bool elfLoadLibrary(int pid, const fs::path& libPath, uintptr_t& outBase);

#endif // __linux__
