// Cover injector_win.cpp static/Win32 functions directly.
// Compiles injector_win.cpp to test: readFileBytes, lastErrorString,
// isQtProcess, generateToken, InitParams layout, and PE helpers.
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
namespace fs = std::filesystem;

#define NOMINMAX
#include <windows.h>
#include <psapi.h>

// Include injector_win.cpp directly to access static functions.
// Override main — injector_win.cpp doesn't have main(), that's in main.cpp.
#include "../../src/injector/injector_win.cpp"

static int passed = 0, total = 0;
#define CHECK(cond, msg) do { total++; if (cond) { passed++; } else { printf("FAIL %s:%d — %s\n", __FILE__, __LINE__, msg); } } while(0)

void test_read_file_bytes_good() {
    char sys[MAX_PATH]; GetSystemDirectoryA(sys, sizeof(sys));
    std::vector<uint8_t> data;
    bool ok = readFileBytes(fs::path(sys) / "kernel32.dll", data);
    CHECK(ok, "readFileBytes kernel32");
    CHECK(data.size() > 65536, "kernel32 > 64KB");
}

void test_read_file_bytes_missing() {
    std::vector<uint8_t> data;
    bool ok = readFileBytes(fs::path("Z:\\nonexistent.dll"), data);
    CHECK(!ok, "readFileBytes missing file fails");
}

void test_last_error_string() {
    // Trigger an error first
    OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, 99999);
    std::string e = lastErrorString();
    CHECK(!e.empty(), "lastErrorString non-empty after failure");
    CHECK(e.find("error") != std::string::npos || e.find("Error") != std::string::npos || !e.empty(), "has content");
}

void test_init_params_size() {
    // Verify InitParams struct is exactly 1024 bytes
    CHECK(sizeof(InitParams) == 1024, "InitParams is 1024 bytes");
}

void test_is_qt_process_self() {
    // Self process — not Qt, should return false
    int self = (int)GetCurrentProcessId();
    bool r = isQtProcess(self);
    CHECK(!r, "self not Qt");
}

void test_is_qt_process_bad_pid() {
    bool r = isQtProcess(99999);
    CHECK(!r, "bad PID returns false");
}

void test_generate_token_format() {
    std::string t = generateToken();
    CHECK(t.size() == 64, "token 64 chars");
    for (char c : t) CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'), "token is hex");
}

void test_rva_to_offset() {
    // Build minimal DOS+NT headers in memory
    std::vector<uint8_t> pe(4096, 0);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(pe.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 64;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(pe.data() + 64);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.NumberOfSections = 1;
    auto* sect = IMAGE_FIRST_SECTION(nt);
    sect->VirtualAddress = 0x1000;
    sect->Misc.VirtualSize = 0x2000;
    sect->PointerToRawData = 0x400;
    DWORD off = rvaToOffset(nt, 0x1000);
    CHECK(off == 0x400, "rvaToOffset maps to file offset");
    DWORD off2 = rvaToOffset(nt, 0x9999);
    CHECK(off2 == 0x9999, "rvaToOffset outside section fallback");
}

int main() {
    printf("=== injector_win.cpp coverage tests ===\n\n");
    test_read_file_bytes_good();
    test_read_file_bytes_missing();
    test_last_error_string();
    test_init_params_size();
    test_is_qt_process_self();
    test_is_qt_process_bad_pid();
    test_generate_token_format();
    test_rva_to_offset();

    printf("\n%d/%d tests passed\n", passed, total);
    return passed == total ? 0 : 1;
}
