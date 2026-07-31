// Test the REAL PE parser in injector_win.cpp against kernel32.dll.
// Includes the source file directly to access static functions.
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

// Pull in static functions from injector_win.cpp
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
// Override the main() from injector_win.cpp to avoid duplicate
// (injector_win.cpp doesn't have main() — that's in main.cpp)
#include "../../src/injector/injector_win.cpp"

static int passed = 0, total = 0;
#define CHECK(cond, msg) do { total++; if (cond) { passed++; } else { printf("FAIL %s:%d — %s\n", __FILE__, __LINE__, msg); } } while(0)

void test_read_file_bytes_real() {
    // Read kernel32.dll (guaranteed to exist)
    char sysdir[MAX_PATH];
    GetSystemDirectoryA(sysdir, sizeof(sysdir));
    fs::path k32 = fs::path(sysdir) / "kernel32.dll";

    std::vector<uint8_t> data;
    bool ok = readFileBytes(k32, data);
    CHECK(ok, "readFileBytes(kernel32.dll) succeeds");
    CHECK(data.size() > 65536, "kernel32.dll > 64KB");
    CHECK(data.size() < 10485760, "kernel32.dll < 10MB");
}

void test_find_export_real() {
    char sysdir[MAX_PATH];
    GetSystemDirectoryA(sysdir, sizeof(sysdir));
    fs::path k32 = fs::path(sysdir) / "kernel32.dll";

    std::vector<uint8_t> data;
    if (!readFileBytes(k32, data)) { CHECK(false, "setup: read kernel32"); return; }

    // GetModuleHandleW must exist in kernel32
    uint32_t rva = findExportRva(data, "GetModuleHandleW");
    CHECK(rva != 0, "findExportRva('GetModuleHandleW') returns non-zero RVA");

    // A non-existent function should return 0
    uint32_t bad = findExportRva(data, "ThisFunctionDoesNotExist_XYZ123");
    CHECK(bad == 0, "findExportRva(nonexistent) returns 0");
}

void test_pe_magic_validation() {
    // Create a buffer without MZ magic
    std::vector<uint8_t> bad(4096, 0);
    uint32_t rva = findExportRva(bad, "anything");
    CHECK(rva == 0, "non-PE data returns 0");
}

void test_pe_no_export_dir() {
    char sysdir[MAX_PATH];
    GetSystemDirectoryA(sysdir, sizeof(sysdir));
    fs::path cmd = fs::path(sysdir) / "cmd.exe";  // small exe, no exports typically

    std::vector<uint8_t> data;
    if (!readFileBytes(cmd, data)) {
        printf("(skip: cannot read cmd.exe)\n");
        return;
    }
    uint32_t rva = findExportRva(data, "anything");
    // cmd.exe typically has no exports — RVA should be 0
    CHECK(rva == 0, "cmd.exe (no exports) returns 0");
}

int main() {
    test_read_file_bytes_real();
    test_find_export_real();
    test_pe_magic_validation();
    test_pe_no_export_dir();

    printf("\n%d/%d tests passed\n", passed, total);
    return passed == total ? 0 : 1;
}
