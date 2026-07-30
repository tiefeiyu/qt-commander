#include "injector.h"

// ---------------------------------------------------------------------------
// macOS stub – real implementation is heavily restricted by SIP (System
// Integrity Protection) and hardened runtime. Even with task_for_pid
// entitlement, injecting into macOS GUI apps (sandboxed) is prevented.
//
// A production implementation would:
//   1. DYLD_INSERT_LIBRARIES via environment variable (launch-time only)
//   2. task_for_pid + mach_vm_allocate + mach_vm_write + thread_create_running
//      (requires com.apple.system-task port entitlement + SIP disabled)
//   3. For init handshake: parse Mach-O export trie or LC_SYMTAB to find
//      qt_commander_init and set up the call via ARM64/x86_64 thread state
// ---------------------------------------------------------------------------

InjectResult injectLibrary(int /*pid*/, const fs::path& /*lib_path*/) {
    return {false, "injectLibrary: not implemented on this platform (macOS)"};
}

InjectResult ejectLibrary(int /*pid*/, const fs::path& /*lib_path*/) {
    return {false, "ejectLibrary: not implemented on this platform (macOS)"};
}

uint16_t performInitHandshake(int /*pid*/, const fs::path& /*lib_path*/,
                              const std::string& /*workspace_path*/,
                              const std::string& /*session_id*/,
                              const std::string& /*token*/,
                              const fs::path& /*port_file_path*/) {
    return 0; // not implemented
}
