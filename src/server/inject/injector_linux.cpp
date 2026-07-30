#include "injector.h"

// ---------------------------------------------------------------------------
// Linux stub – real implementation requires ptrace-based shellcode injection
// or /proc/<pid>/mem manipulation, which needs CAP_SYS_PTRACE or ptrace
// scope adjustments. This is a non-trivial security boundary.
//
// A production implementation would:
//   1. ptrace(PTRACE_ATTACH, pid) or use process_vm_writev
//   2. Allocate memory in the target (via mmap through syscall injection)
//   3. Write the library path and a small trampoline calling dlopen
//   4. Detach and let the target continue
//   5. For init handshake: parse ELF .dynsym to find qt_commander_init
//      and construct a call via shellcode
// ---------------------------------------------------------------------------

InjectResult injectLibrary(int /*pid*/, const fs::path& /*lib_path*/) {
    return {false, "injectLibrary: not implemented on this platform (Linux)"};
}

InjectResult ejectLibrary(int /*pid*/, const fs::path& /*lib_path*/) {
    return {false, "ejectLibrary: not implemented on this platform (Linux)"};
}

uint16_t performInitHandshake(int /*pid*/, const fs::path& /*lib_path*/,
                              const std::string& /*workspace_path*/,
                              const std::string& /*session_id*/,
                              const std::string& /*token*/,
                              const fs::path& /*port_file_path*/) {
    return 0; // not implemented
}
