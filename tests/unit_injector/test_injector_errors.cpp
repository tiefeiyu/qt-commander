// Trigger injectLibrary error paths in injector_win.cpp via fake DLL.
// Requires a real Qt process PID (uses qt-widget-test).
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>
#include <thread>
#include <chrono>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>

#include "../../src/injector/injector.h"

static int passed = 0, total = 0;
#define CHECK(cond, msg) do { total++; if (cond) { passed++; } else { printf("FAIL %s:%d — %s\n", __FILE__, __LINE__, msg); } } while(0)

static DWORD find_qt_pid() {
    // Try finding qt-widget-test.exe
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe = {sizeof(pe)}; DWORD pid = 0;
    if (Process32First(s, &pe)) {
        do { if (_stricmp(pe.szExeFile, "qt-widget-test.exe") == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32Next(s, &pe));
    }
    CloseHandle(s);
    return pid;
}

static int run_injector(const std::string& ip, DWORD pid, const std::string& lp, const std::string& pf) {
    std::string ic = "\"" + ip + "\" " + std::to_string(pid) + " \"" + lp + "\" \"" + pf + "\"";
    STARTUPINFO si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
    if (!CreateProcess(nullptr, (LPSTR)ic.c_str(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD ec = 99; GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)ec;
}

void test_inject_fake_dll() {
    // 1. Ensure Qt test app is running
    STARTUPINFO si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
    std::string tp = "build/msvc/tests/test-apps/widget/qt-widget-test.exe";
    if (GetFileAttributesA(tp.c_str()) == INVALID_FILE_ATTRIBUTES) {
        tp = "build\\msvc\\tests\\test-apps\\widget\\qt-widget-test.exe";
    }
    if (!CreateProcess(nullptr, (LPSTR)tp.c_str(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        // Try alternate locations
        const char* alts[] = {"./qt-widget-test.exe", "build\\msvc\\qt-widget-test.exe", nullptr};
        bool found = false;
        for (int i = 0; alts[i] && !found; i++) {
            if (GetFileAttributesA(alts[i]) != INVALID_FILE_ATTRIBUTES) {
                if (CreateProcess(nullptr, (LPSTR)alts[i], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
                    found = true;
            }
        }
        if (!found) { CHECK(false, "Cannot launch Qt test app — SKIP"); return; }
    }
    DWORD qt_pid = pi.dwProcessId;
    printf("Qt PID: %lu\n", qt_pid);
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // 2. Create a fake DLL file that exists but is not valid PE
    std::string fake_dll = std::string(getenv("TEMP") ? getenv("TEMP") : ".") + "/fake_lib.dll";
    std::ofstream f(fake_dll, std::ios::binary);
    f << "This is not a valid DLL file. LoadLibraryW will fail."; f.close();
    CHECK(GetFileAttributesA(fake_dll.c_str()) != INVALID_FILE_ATTRIBUTES, "fake DLL created");

    // 3. Find coverage injector
    std::string ip = "build/qt-injector_cov.exe";
    if (GetFileAttributesA(ip.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ip = "build\\qt-injector_cov.exe";
    }

    std::string pf = std::string(getenv("TEMP") ? getenv("TEMP") : ".") + "/err_port.txt";
    DeleteFileA(pf.c_str());

    // 4. Run injector — expects LoadLibraryW to fail with fake DLL
    int ec = run_injector(ip, qt_pid, fake_dll, pf);
    printf("Exit code with fake DLL: %d\n", ec);
    // LoadLibraryW fails → GetExitCodeThread returns 0 → injectLibrary returns error → exit 2
    CHECK(ec == 2, (std::string("fake DLL → exit 2 (LoadLibraryW failure), got ") + std::to_string(ec)).c_str());

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
