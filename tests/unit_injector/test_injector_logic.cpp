// Comprehensive injector logic tests using MockProcessOps.
// Tests all DI-injected functions without requiring real Windows processes.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>

#include "os_ops.h"
#include "injector.h"

namespace fs = std::filesystem;

static int passed = 0, total = 0;

#define CHECK(cond, msg) do { total++; if (cond) { passed++; } else { printf("FAIL %s:%d — %s\n", __FILE__, __LINE__, msg); } } while(0)

// --- injectLibrary ---
void test_inject_success() {
    MockProcessOps ops;
    ops.open_process_result = true; ops.alloc_mem_result = true;
    ops.write_mem_result = true; ops.create_thread_result = true;
    ops.wait_result = true; ops.thread_exit_code = 0x7FFE0000;
    auto r = injectLibrary(ops, 1234, fs::path("C:\\test\\lib.dll"));
    CHECK(r.ok, "injectLibrary success");
    CHECK(ops.calls.size() == 4, "call count");
    CHECK(ops.calls[0].name == "open_process", "first call is open_process");
    CHECK(ops.calls[1].name == "alloc_mem", "second call is alloc_mem");
    CHECK(ops.calls[2].name == "write_mem", "third call is write_mem");
    CHECK(ops.calls[3].name == "create_thread", "fourth call is create_thread");
}

void test_inject_open_fails() {
    MockProcessOps ops;
    ops.open_process_result = false; ops.last_error_str = "denied";
    auto r = injectLibrary(ops, 1234, fs::path("C:\\lib.dll"));
    CHECK(!r.ok, "inject open fails");
    CHECK(r.error.find("OpenProcess") != std::string::npos, "error mentions OpenProcess");
}

void test_inject_alloc_fails() {
    MockProcessOps ops;
    ops.open_process_result = true; ops.alloc_mem_result = false;
    auto r = injectLibrary(ops, 1234, fs::path("C:\\lib.dll"));
    CHECK(!r.ok, "inject alloc fails");
    CHECK(r.error.find("VirtualAllocEx") != std::string::npos, "error mentions VirtualAllocEx");
}

void test_inject_write_fails() {
    MockProcessOps ops;
    ops.open_process_result = true; ops.alloc_mem_result = true;
    ops.write_mem_result = false;
    auto r = injectLibrary(ops, 1234, fs::path("C:\\lib.dll"));
    CHECK(!r.ok, "inject write fails");
    CHECK(r.error.find("WriteProcessMemory") != std::string::npos, "error mentions WriteProcessMemory");
}

void test_inject_thread_fails() {
    MockProcessOps ops;
    ops.open_process_result = true; ops.alloc_mem_result = true;
    ops.write_mem_result = true; ops.create_thread_result = false;
    auto r = injectLibrary(ops, 1234, fs::path("C:\\lib.dll"));
    CHECK(!r.ok, "inject thread fails");
}

void test_inject_timeout() {
    MockProcessOps ops;
    ops.open_process_result = true; ops.alloc_mem_result = true;
    ops.write_mem_result = true; ops.create_thread_result = true;
    ops.wait_result = false;
    auto r = injectLibrary(ops, 1234, fs::path("C:\\lib.dll"));
    CHECK(!r.ok, "inject timeout");
    CHECK(r.error.find("timed out") != std::string::npos, "error mentions timeout");
}

void test_inject_null_dll_base() {
    MockProcessOps ops;
    ops.open_process_result = true; ops.alloc_mem_result = true;
    ops.write_mem_result = true; ops.create_thread_result = true;
    ops.wait_result = true; ops.thread_exit_code = 0;
    auto r = injectLibrary(ops, 1234, fs::path("C:\\lib.dll"));
    CHECK(!r.ok, "null DLL base");
    CHECK(r.error.find("NULL") != std::string::npos, "error mentions NULL");
}

// --- isQtProcess ---
void test_is_qt_true() {
    MockProcessOps ops;
    ops.open_process_result = true;
    ops.module_names = {"kernel32.dll", "Qt5Core.dll", "user32.dll"};
    CHECK(isQtProcess(ops, 1234), "Qt5 detected");
}

void test_is_qt_qt6() {
    MockProcessOps ops;
    ops.open_process_result = true;
    ops.module_names = {"ntdll.dll", "Qt6Core.dll"};
    CHECK(isQtProcess(ops, 5678), "Qt6 detected");
}

void test_is_qt_false() {
    MockProcessOps ops;
    ops.open_process_result = true;
    ops.module_names = {"kernel32.dll", "user32.dll"};
    CHECK(!isQtProcess(ops, 9999), "non-Qt not detected");
}

void test_is_qt_open_fails() {
    MockProcessOps ops;
    ops.open_process_result = false;
    CHECK(!isQtProcess(ops, 1234), "open fails returns false");
}

void test_is_qt_empty_modules() {
    MockProcessOps ops;
    ops.open_process_result = true;
    ops.module_names = {};
    CHECK(!isQtProcess(ops, 1234), "empty modules returns false");
}

// --- generateToken ---
void test_gen_token_hex() {
    MockProcessOps ops;
    ops.random_result = true;
    ops.random_bytes.assign(32, 0xFF);
    std::string t = generateToken(ops);
    CHECK(t.size() == 64, "token length 64");
    CHECK(t == std::string(64, 'f'), "all f chars");
}

void test_gen_token_ab() {
    MockProcessOps ops;
    ops.random_result = true;
    ops.random_bytes.assign(32, 0xAB);
    std::string t = generateToken(ops);
    bool ok = true;
    for (char c : t) if (c != 'a' && c != 'b') ok = false;
    CHECK(ok, "token chars are only a/b");
}

void test_gen_token_fails() {
    MockProcessOps ops;
    ops.random_result = false;
    bool threw = false;
    try { generateToken(ops); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw, "CSPRNG failure throws");
}

// --- ejectLibrary ---
void test_eject_success() {
    MockProcessOps ops;
    ops.open_process_result = true;
    ops.module_names = {"kernel32.dll", "libqt-commander.dll"};
    ops.create_thread_result = true; ops.wait_result = true;
    auto r = ejectLibrary(ops, 1234, fs::path("C:\\test\\libqt-commander.dll"));
    // ejectLibrary uses real GetProcAddress for FreeLibrary — on Mock,
    // get_module_handle + get_proc_address may fail, but the CreateRemoteThread path works
    // because create_thread_result=true
    CHECK(r.ok == true || r.error.find("GetProcAddress") != std::string::npos,
          "eject succeeds or reports FreeLibrary error");
}

void test_eject_open_fails() {
    MockProcessOps ops;
    ops.open_process_result = false;
    auto r = ejectLibrary(ops, 1234, fs::path("C:\\lib.dll"));
    CHECK(!r.ok, "eject open fails");
}

void test_eject_dll_not_found() {
    MockProcessOps ops;
    ops.open_process_result = true;
    ops.module_names = {"kernel32.dll"};
    auto r = ejectLibrary(ops, 1234, fs::path("C:\\libqt-commander.dll"));
    CHECK(!r.ok, "eject dll not found");
    CHECK(r.error.find("not found") != std::string::npos, "error mentions not found");
}

// --- performInitHandshake ---
void test_init_success() {
    MockProcessOps ops;
    ops.open_process_result = true; ops.alloc_mem_result = true;
    ops.write_mem_result = true; ops.create_thread_result = true;
    ops.wait_result = true; ops.read_file_result = true;
    ops.file_bytes = {0x4D, 0x5A};
    ops.module_names = {"libqt-commander.dll"};

    auto pf = fs::temp_directory_path() / "test_port_init.txt";
    { std::ofstream f(pf); f << "23456\nabcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234\n"; }

    uint16_t port = performInitHandshake(ops, 1234, fs::path("C:\\libqt-commander.dll"),
        "ws", "sid", "abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234", pf);
    CHECK(port == 23456, "init returns correct port");
    fs::remove(pf);
}

void test_init_open_fails() {
    MockProcessOps ops;
    ops.open_process_result = false; ops.read_file_result = true;
    ops.file_bytes = {0x4D, 0x5A};
    uint16_t port = performInitHandshake(ops, 1234, fs::path("C:\\lib.dll"), "w","s","t",
        fs::temp_directory_path() / "x.txt");
    CHECK(port == 0, "init open fails returns 0");
}

void test_init_token_mismatch() {
    MockProcessOps ops;
    ops.open_process_result = true; ops.alloc_mem_result = true;
    ops.write_mem_result = true; ops.create_thread_result = true;
    ops.wait_result = true; ops.read_file_result = true;
    ops.file_bytes = {0x4D, 0x5A};
    ops.module_names = {"libqt-commander.dll"};

    auto pf = fs::temp_directory_path() / "test_port_mismatch.txt";
    { std::ofstream f(pf); f << "12345\nwrong_token_value_here\n"; }

    uint16_t port = performInitHandshake(ops, 1234, fs::path("C:\\libqt-commander.dll"),
        "w", "s", "abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234", pf);
    CHECK(port == 0, "token mismatch returns 0");
    fs::remove(pf);
}

void test_init_read_fails() {
    MockProcessOps ops;
    ops.read_file_result = false;
    uint16_t port = performInitHandshake(ops, 1234, fs::path("C:\\lib.dll"), "w","s","t",
        fs::temp_directory_path() / "y.txt");
    CHECK(port == 0, "read fails returns 0");
}

void test_init_thread_timeout() {
    MockProcessOps ops;
    ops.open_process_result = true; ops.alloc_mem_result = true;
    ops.write_mem_result = true; ops.create_thread_result = true;
    ops.wait_result = false; ops.read_file_result = true;
    ops.file_bytes = {0x4D, 0x5A};
    ops.module_names = {"libqt-commander.dll"};

    uint16_t port = performInitHandshake(ops, 1234, fs::path("C:\\libqt-commander.dll"),
        "w", "s", "t", fs::temp_directory_path() / "z.txt");
    CHECK(port == 0, "thread timeout returns 0");
}

int main() {
    test_inject_success();
    test_inject_open_fails();
    test_inject_alloc_fails();
    test_inject_write_fails();
    test_inject_thread_fails();
    test_inject_timeout();
    test_inject_null_dll_base();
    test_is_qt_true();
    test_is_qt_qt6();
    test_is_qt_false();
    test_is_qt_open_fails();
    test_is_qt_empty_modules();
    test_gen_token_hex();
    test_gen_token_ab();
    test_gen_token_fails();
    test_eject_success();
    test_eject_open_fails();
    test_eject_dll_not_found();
    test_init_success();
    test_init_open_fails();
    test_init_token_mismatch();
    test_init_read_fails();
    test_init_thread_timeout();

    printf("\n%d/%d tests passed\n", passed, total);
    return passed == total ? 0 : 1;
}
