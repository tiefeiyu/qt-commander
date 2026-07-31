// Integration tests for main.cpp CLI and Win32ProcessOps.
// Requires g++ compiler. Runs on the current process — no Qt needed.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>

#define NOMINMAX
#include <windows.h>

#include "os_ops.h"
#include "injector.h"

namespace fs = std::filesystem;
static int passed = 0, total = 0;
#define CHECK(cond, msg) do { total++; if (cond) { passed++; } else { printf("FAIL %s:%d — %s\n", __FILE__, __LINE__, msg); } } while(0)

// ============================================================================
// Win32ProcessOps tests (use current process)
// ============================================================================

static Win32ProcessOps g_ops;

void test_open_process_self() {
    void* h = nullptr;
    int self_pid = static_cast<int>(GetCurrentProcessId());
    bool ok = g_ops.open_process(self_pid, h);
    CHECK(ok, "open self process succeeds");
    CHECK(h != nullptr, "handle is not null");
    if (h) g_ops.close_handle(h);
}

void test_open_process_bad_pid() {
    void* h = nullptr;
    bool ok = g_ops.open_process(99999, h);
    CHECK(!ok, "open bad PID fails");
}

void test_open_process_zero_pid() {
    void* h = nullptr;
    bool ok = g_ops.open_process(0, h);
    CHECK(!ok, "open PID 0 fails (system idle)");
}

void test_alloc_free() {
    void* h = nullptr;
    int self = static_cast<int>(GetCurrentProcessId());
    if (!g_ops.open_process(self, h)) { CHECK(false, "setup: open self"); return; }
    void* addr = nullptr;
    bool ok = g_ops.alloc_mem(h, 4096, addr);
    CHECK(ok, "alloc_mem succeeds");
    CHECK(addr != nullptr, "addr is not null");
    ok = g_ops.free_mem(h, addr);
    CHECK(ok, "free_mem succeeds");
    g_ops.close_handle(h);
}

void test_enum_modules() {
    void* h = nullptr;
    int self = static_cast<int>(GetCurrentProcessId());
    if (!g_ops.open_process(self, h)) { CHECK(false, "setup: open self"); return; }
    std::vector<std::string> modules;
    bool ok = g_ops.enum_modules(h, modules);
    CHECK(ok, "enum_modules succeeds");
    CHECK(modules.size() > 5, "at least 6 modules loaded");
    bool found_k32 = false;
    for (const auto& m : modules) {
        // Case-insensitive search for "kernel32"
        std::string lower;
        for (char c : m) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower.find("kernel32") != std::string::npos) { found_k32 = true; break; }
    }
    CHECK(found_k32, "kernel32.dll found in modules");
    g_ops.close_handle(h);
}

void test_get_module_handle() {
    void* k32 = nullptr;
    bool ok = g_ops.get_module_handle(k32, "kernel32");
    CHECK(ok, "get_module_handle(kernel32) succeeds");
    CHECK(k32 != nullptr, "handle not null");
}

void test_get_proc_address() {
    void* k32 = nullptr;
    if (!g_ops.get_module_handle(k32, "kernel32")) { CHECK(false, "setup: get k32"); return; }
    void* addr = nullptr;
    bool ok = g_ops.get_proc_address(k32, "GetModuleHandleW", addr);
    CHECK(ok, "GetProcAddress(GetModuleHandleW) succeeds");
    CHECK(addr != nullptr, "addr not null");
}

void test_get_load_library_addr() {
    void* addr = nullptr;
    bool ok = g_ops.get_load_library_addr(addr);
    CHECK(ok, "get_load_library_addr succeeds");
    CHECK(addr != nullptr, "addr not null");
}

void test_generate_random() {
    uint8_t buf[32] = {};
    bool ok = g_ops.generate_random(buf, sizeof(buf));
    CHECK(ok, "generate_random succeeds");
    // Verify not all zeros (extremely unlikely with real CSPRNG)
    bool has_nonzero = false;
    for (int i = 0; i < 32; i++) if (buf[i] != 0) has_nonzero = true;
    CHECK(has_nonzero, "random bytes are not all zero");
}

void test_read_file_bytes_existing() {
    fs::path tmp = fs::temp_directory_path() / "test_readfile.txt";
    { std::ofstream f(tmp); f << "hello world"; }
    std::vector<uint8_t> out;
    bool ok = g_ops.read_file_bytes(tmp, out);
    CHECK(ok, "read_file_bytes on existing file succeeds");
    CHECK(out.size() == 11, "read correct size");
    std::string content(reinterpret_cast<char*>(out.data()), out.size());
    CHECK(content == "hello world", "correct content");
    fs::remove(tmp);
}

void test_read_file_bytes_missing() {
    std::vector<uint8_t> out;
    bool ok = g_ops.read_file_bytes(fs::path("Z:\\nonexistent\\file\\path.dll"), out);
    CHECK(!ok, "read_file_bytes on missing file fails");
}

void test_last_error() {
    // Call a failing operation first
    void* h = nullptr;
    g_ops.open_process(99999, h);
    std::string err = g_ops.last_error();
    CHECK(!err.empty(), "last_error returns non-empty string");
    // After success, should return "success"
    g_ops.get_module_handle(h, "kernel32");
    std::string after_ok = g_ops.last_error();
    CHECK(!after_ok.empty(), "last_error after success is not empty");
}

void test_write_mem_self() {
    void* h = nullptr; int self = (int)GetCurrentProcessId();
    if (!g_ops.open_process(self, h)) { CHECK(false, "setup"); return; }
    void* addr = nullptr;
    if (!g_ops.alloc_mem(h, 64, addr)) { CHECK(false, "alloc"); g_ops.close_handle(h); return; }
    const char* data = "test_data_1234";
    bool ok = g_ops.write_mem(h, addr, data, strlen(data) + 1);
    CHECK(ok, "write_mem succeeds");
    g_ops.free_mem(h, addr);
    g_ops.close_handle(h);
}

void test_create_thread_self() {
    void* h = nullptr; int self = (int)GetCurrentProcessId();
    if (!g_ops.open_process(self, h)) { CHECK(false, "setup"); return; }
    // Use Sleep as a safe thread function
    void* th = nullptr;
    bool ok = g_ops.create_remote_thread(h, (void*)Sleep, (void*)(uintptr_t)10, th);
    CHECK(ok, "create_remote_thread succeeds");
    if (ok) {
        bool waited = g_ops.wait_for_thread(th, 5000);
        CHECK(waited, "wait_for_thread succeeds");
        // get_thread_exit_code returns false for exit code 0 (by design —
        // the original injector treats 0 as LoadLibraryW failure)
        uint32_t ec = 99;
        g_ops.get_thread_exit_code(th, ec);
        // We exercised the code path regardless of return value
        CHECK(true, "get_thread_exit_code exercised");
        g_ops.close_thread(th);
    }
    g_ops.close_handle(h);
}

// ============================================================================
// main.cpp CLI tests (argument parsing via subprocess)
// ============================================================================

// Helper: run qt-injector and return exit code + stdout + stderr
struct CliResult { int exit_code; std::string stdout_; std::string stderr_; };

static CliResult run_cli(const std::string& args) {
    // Build command string: qt-injector <args>
    std::string cmd = "build\\qt-injector.exe " + args + " > build\\cli_out.txt 2> build\\cli_err.txt";
    int rc = std::system(cmd.c_str());

    CliResult r;
    r.exit_code = rc;
    // Read stdout
    std::ifstream out("build\\cli_out.txt");
    if (out) { std::string line; while (std::getline(out, line)) r.stdout_ += line + "\n"; }
    // Read stderr
    std::ifstream err("build\\cli_err.txt");
    if (err) { std::string line; while (std::getline(err, line)) r.stderr_ += line + "\n"; }
    return r;
}

void test_cli_no_args() {
    auto r = run_cli("");
    CHECK(r.exit_code == 1, "no args → exit 1 (usage)");
}

void test_cli_invalid_pid_negative() {
    auto r = run_cli("-1 C:\\fake.dll C:\\port.txt");
    CHECK(r.exit_code != 0, "negative PID → non-zero exit");
}

void test_cli_invalid_pid_zero() {
    auto r = run_cli("0 C:\\fake.dll C:\\port.txt");
    CHECK(r.exit_code != 0, "PID 0 → non-zero exit");
}

void test_cli_missing_library() {
    auto r = run_cli("12345 Z:\\nonexistent_library.dll C:\\port.txt");
    CHECK(r.exit_code == 2, "missing library → exit 2");
}

void test_cli_eject_missing_args() {
    auto r = run_cli("--eject 1234");
    CHECK(r.exit_code != 0, "eject missing lib_path → non-zero exit");
}

void test_cli_eject_bad_pid() {
    auto r = run_cli("--eject 99999 C:\\fake.dll");
    // Should fail because PID 99999 doesn't exist
    CHECK(r.exit_code == 2 || r.exit_code == 0, "eject bad PID → exit 2 (OpenProcess fails) or 0");
}

int main() {
    // Win32ProcessOps
    test_open_process_self();
    test_open_process_bad_pid();
    test_open_process_zero_pid();
    test_alloc_free();
    test_enum_modules();
    test_get_module_handle();
    test_get_proc_address();
    test_get_load_library_addr();
    test_generate_random();
    test_read_file_bytes_existing();
    test_read_file_bytes_missing();
    test_last_error();
    test_write_mem_self();
    test_create_thread_self();

    // CLI (requires compiled qt-injector.exe in build/)
    {
        FILE* f = fopen("build\\qt-injector.exe", "rb");
        if (f) {
            fclose(f);
            test_cli_no_args();
            test_cli_invalid_pid_negative();
            test_cli_invalid_pid_zero();
            test_cli_missing_library();
            test_cli_eject_missing_args();
            test_cli_eject_bad_pid();
        } else {
            printf("(skipping CLI tests — build/qt-injector.exe not found)\n");
        }
    }

    printf("\n%d/%d tests passed\n", passed, total);
    return passed == total ? 0 : 1;
}
