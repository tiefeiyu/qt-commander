// Test exit codes 4 and 5 using --sleep-before-check flag.
// Exit 4: delete port file during sleep window.
// Exit 5: modify port file token during sleep window.
#include <cstdio>
#include <cstdlib>
#include <string>
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

static int passed = 0, total = 0;
#define CHECK(cond, msg) do { total++; if (cond) { passed++; } else { printf("FAIL %s:%d — %s\n", __FILE__, __LINE__, msg); } } while(0)

static int run_cov_injector(DWORD pid, const std::string& lib, const std::string& pf, int sleep_ms) {
    std::string cmd = "build\\qt-injector_cov.exe " + std::to_string(pid) + " \"" + lib + "\" \"" + pf + "\" --sleep-before-check " + std::to_string(sleep_ms);
    STARTUPINFO si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
    if (!CreateProcess(nullptr, (LPSTR)cmd.c_str(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD ec = 99; GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)ec;
}

void test_exit_4_delete_port_file() {
    // Launch Qt test app
    STARTUPINFO si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
    const char* tp = "build\\msvc\\tests\\test-apps\\widget\\qt-widget-test.exe";
    if (!CreateProcess(nullptr, (LPSTR)tp, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CHECK(false, "launch Qt test app"); return;
    }
    DWORD pid = pi.dwProcessId;
    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::string lib = "build\\msvc\\src\\library\\libqt-commander.dll";
    std::string pf = std::string(getenv("TEMP") ? getenv("TEMP") : ".") + "\\e2e_exit4.txt";
    DeleteFileA(pf.c_str());

    // Run injector with 3s sleep window
    std::thread killer([&pf]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        DeleteFileA(pf.c_str());
    });

    int ec = run_cov_injector(pid, lib, pf, 3000);
    killer.join();

    printf("Exit code with port file deleted: %d\n", ec);
    CHECK(ec == 4, (std::string("exit 4 (port file deleted), got ") + std::to_string(ec)).c_str());

    TerminateProcess(pi.hProcess, 0); WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
}

void test_exit_5_wrong_token() {
    // Launch Qt test app
    STARTUPINFO si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
    const char* tp = "build\\msvc\\tests\\test-apps\\widget\\qt-widget-test.exe";
    if (!CreateProcess(nullptr, (LPSTR)tp, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CHECK(false, "launch Qt test app"); return;
    }
    DWORD pid = pi.dwProcessId;
    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::string lib = "build\\msvc\\src\\library\\libqt-commander.dll";
    std::string pf = std::string(getenv("TEMP") ? getenv("TEMP") : ".") + "\\e2e_exit5.txt";
    DeleteFileA(pf.c_str());

    // Run injector with 3s sleep window; during sleep, modify token in port file
    std::thread modifier([&pf]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        // Wait for port file to appear (library writes it), then corrupt token
        for (int i = 0; i < 20; i++) {
            std::ifstream check(pf);
            if (check.is_open()) {
                check.close();
                // Read current content, replace second line with wrong token
                std::ifstream in(pf);
                std::string port_line, token_line;
                std::getline(in, port_line);
                std::getline(in, token_line);
                in.close();
                std::ofstream out(pf);
                out << port_line << "\n" << "wrong_token_0000000000000000000000000000000000000000000000000000\n";
                out.close();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });

    int ec = run_cov_injector(pid, lib, pf, 3000);
    modifier.join();

    printf("Exit code with wrong token: %d\n", ec);
    CHECK(ec == 5, (std::string("exit 5 (token mismatch), got ") + std::to_string(ec)).c_str());

    TerminateProcess(pi.hProcess, 0); WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
}

int main() {
    printf("=== Exit 4/5 Coverage Tests ===\n\n");
    test_exit_4_delete_port_file();
    test_exit_5_wrong_token();
    printf("\n%d/%d tests passed\n", passed, total);
    return passed == total ? 0 : 1;
}
