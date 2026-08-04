// E2E exit code coverage: test exit codes 3 and 6.
// Exit 6: non-Qt process → isQtProcess fails
// Exit 3: unwritable port file path → init times out polling
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include "test_util.h"

static int passed = 0, total = 0;
#define CHECK(cond, msg) do { total++; if (cond) { passed++; } else { printf("FAIL %s:%d — %s\n", __FILE__, __LINE__, msg); } } while(0)

static std::string discover(const char* name) {
    const char* bases[] = {
        ".", "..",
        "build", "build/msvc",
        "tests/test-apps/widget", "tests",
        "../src/library", "../tests/test-apps/widget",
        "build/msvc/tests/test-apps/widget",
        "build/msvc/src/library", "src/library",
        nullptr
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

static DWORD find_proc(const char* name) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe = {sizeof(pe)}; DWORD pid = 0;
    if (Process32First(s, &pe))
        do { if (_stricmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32Next(s, &pe));
    CloseHandle(s); return pid;
}

static int run_injector(const std::string& ip, const std::string& lp,
                         DWORD pid, const std::string& pf, DWORD timeout_ms = 120000) {
    DeleteFileA(pf.c_str());
    std::string ic = "\"" + ip + "\" " + std::to_string(pid) + " \"" + lp + "\" \"" + pf + "\"";
    STARTUPINFO si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
    if (!CreateProcess(nullptr, (LPSTR)ic.c_str(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, timeout_ms);
    DWORD ec = 99; GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)ec;
}

void test_exit_6_non_qt_process() {
    // Launch cmd.exe and inject into it — isQtProcess should detect it's not Qt
    STARTUPINFO si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
    if (!CreateProcess(nullptr, (LPSTR)"cmd.exe /c timeout /t 5 /nobreak >nul 2>&1",
                       nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CHECK(false, "launch cmd.exe"); return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::string ip = discover("qt-injector.exe");
    std::string lp = discover("libqt-commander.dll");
    std::string pf = std::string(getenv("TEMP") ? getenv("TEMP") : ".") + "/e2e_x6.txt";

    if (ip.empty() || lp.empty()) {
        printf("SKIP: binaries not found\n");
        TerminateProcess(pi.hProcess, 0); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        return;
    }

    int ec = run_injector(ip, lp, pi.dwProcessId, pf, 15000);
    CHECK(ec == 6, (std::string("exit 6 (non-Qt), got ") + std::to_string(ec)).c_str());

    TerminateProcess(pi.hProcess, 0); WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    DeleteFileA(pf.c_str());
}

void test_exit_3_unwritable_port_file() {
    // Use real Qt test app, but port file in non-existent directory
    std::string tp = discover("qt-widget-test.exe");
    if (tp.empty()) { printf("SKIP: test app not found\n"); return; }

    STARTUPINFO si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
    if (!CreateProcess(nullptr, (LPSTR)tp.c_str(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CHECK(false, "launch test app"); return;
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));

    std::string ip = discover("qt-injector.exe");
    std::string lp = discover("libqt-commander.dll");
    // Port file in a path that doesn't exist — DLL can't create it
    std::string pf = "Z:\\nonexistent_dir_xyz\\port.txt";

    if (ip.empty() || lp.empty()) {
        printf("SKIP: binaries not found\n");
        TerminateProcess(pi.hProcess, 0); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        return;
    }

    int ec = run_injector(ip, lp, pi.dwProcessId, pf, 60000);
    // Exit 3 = init failed/timed out because port file could not be created
    CHECK(ec == 3, (std::string("exit 3 (init failed), got ") + std::to_string(ec)).c_str());

    TerminateProcess(pi.hProcess, 0); WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
}

int main() {
    chdir_to_exe_dir();          // anchor CWD to this exe's build tree
    printf("=== Exit Code Coverage Tests ===\n\n");
    test_exit_6_non_qt_process();
    test_exit_3_unwritable_port_file();
    printf("\n%d/%d tests passed\n", passed, total);
    return passed == total ? 0 : 1;
}
