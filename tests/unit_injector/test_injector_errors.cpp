// Trigger injectLibrary error paths via fake DLL.
// Runs qt-injector.exe as subprocess with an invalid DLL.
#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdlib>
#include <string>
#include <fstream>
#include <thread>
#include <chrono>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

static int passed = 0, total = 0;
#define CHECK(cond, msg) do { total++; if (cond) { passed++; } else { printf("FAIL %s:%d — %s\n", __FILE__, __LINE__, msg); } } while(0)

// Auto-discover binary paths (same logic as test_e2e.cpp)
static std::string find_binary(const char* name) {
    const char* bases[] = {
        ".", "..",                           // binary-dir or parent (ctest)
        "build", "build/msvc",
        "tests/test-apps/widget", "tests",
        "../src/library", "../tests/test-apps/widget",
        "build/msvc/tests/test-apps/widget",
        "build/msvc/src/library", "src/library",
        "build/msvc/src/injector/Release",
        "../build/msvc", nullptr
    };
    for (int i = 0; bases[i]; i++) {
        std::string p = std::string(bases[i]) + "/" + name;
        if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
            // CreateProcess cannot reliably resolve relative paths with
            // directory separators; return an absolute path.
            char full[MAX_PATH];
            if (GetFullPathNameA(p.c_str(), MAX_PATH, full, nullptr) > 0)
                return full;
            return p;
        }
    }
    return "";
}

static int run_injector(const std::string& ip, DWORD pid, const std::string& lp, const std::string& pf) {
    std::string ic = "\"" + ip + "\" " + std::to_string(pid) + " \"" + lp + "\" \"" + pf + "\"";
    STARTUPINFOA si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, (LPSTR)ic.c_str(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD ec = 99; GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)ec;
}

void test_inject_fake_dll() {
    // 1. Launch Qt test app
    std::string tp = find_binary("qt-widget-test.exe");
    if (tp.empty()) { printf("SKIP: qt-widget-test.exe not found\n"); return; }

    STARTUPINFOA si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, (LPSTR)tp.c_str(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CHECK(false, "Cannot launch Qt test app"); return;
    }
    DWORD qt_pid = pi.dwProcessId;
    printf("Qt PID: %lu\n", qt_pid);
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // 2. Create a fake DLL file that exists but is not valid PE
    std::string fake_dll = std::string(getenv("TEMP") ? getenv("TEMP") : ".") + "/fake_lib.dll";
    std::ofstream f(fake_dll, std::ios::binary);
    f << "This is not a valid DLL file. LoadLibraryW will fail."; f.close();
    CHECK(GetFileAttributesA(fake_dll.c_str()) != INVALID_FILE_ATTRIBUTES, "fake DLL created");

    // 3. Find injector
    std::string ip = find_binary("qt-injector.exe");
    if (ip.empty()) { printf("SKIP: qt-injector.exe not found\n"); DeleteFileA(fake_dll.c_str()); TerminateProcess(pi.hProcess, 0); WaitForSingleObject(pi.hProcess, 5000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); return; }

    std::string pf = std::string(getenv("TEMP") ? getenv("TEMP") : ".") + "/err_port.txt";
    DeleteFileA(pf.c_str());

    // 4. Run injector — expects LoadLibraryW to fail with fake DLL
    int ec = run_injector(ip, qt_pid, fake_dll, pf);
    printf("Exit code with fake DLL: %d\n", ec);
    // LoadLibraryW fails → GetExitCodeThread returns 0 → injectLibrary returns error → exit 2
    CHECK(ec == 2, (std::string("fake DLL -> exit 2 (LoadLibraryW failure), got ") + std::to_string(ec)).c_str());

    // 5. Cleanup
    TerminateProcess(pi.hProcess, 0); WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    DeleteFileA(fake_dll.c_str()); DeleteFileA(pf.c_str());
}

int main() {
    printf("=== injectLibrary error path coverage ===\n\n");
    test_inject_fake_dll();
    printf("\n%d/%d tests passed\n", passed, total);
    return passed == total ? 0 : 1;
}
