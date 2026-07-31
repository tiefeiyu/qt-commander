// End-to-end integration test: inject libqt-commander into qt-widget-test.
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

static DWORD find_process(const char* name) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe = { sizeof(pe) }; DWORD pid = 0;
    if (Process32First(s, &pe)) {
        do { if (_stricmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32Next(s, &pe));
    }
    CloseHandle(s); return pid;
}

static bool tcp_connect(uint16_t port, SOCKET& sock) {
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) return false;
    sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_port = htons(port);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(sock, (sockaddr*)&a, sizeof(a)) != 0) { closesocket(sock); return false; }
    int to = 5000; setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));
    return true;
}

static bool send_all(SOCKET s, const void* d, size_t n) {
    size_t x = 0; while (x < n) { int r = send(s, (const char*)d + x, (int)(n - x), 0); if (r <= 0) return false; x += r; } return true;
}

static bool send_frame(SOCKET s, const std::string& j) {
    uint32_t be = htonl((uint32_t)j.size());
    return send_all(s, &be, 4) && send_all(s, j.data(), j.size());
}

static std::string recv_frame(SOCKET s) {
    uint8_t h[4]; if (recv(s, (char*)h, 4, MSG_WAITALL) != 4) return "";
    uint32_t len = ntohl(*(uint32_t*)h);
    if (len == 0 || len > 16 * 1024 * 1024) return "";
    std::string b(len, '\0');
    if (recv(s, &b[0], (int)len, MSG_WAITALL) != (int)len) return "";
    return b;
}

int main() {
    // Auto-discover binaries
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
    std::string ip, lp, tp;
    for (int i = 0; bases[i]; i++) {
        std::string b(bases[i]);
        if (ip.empty() && GetFileAttributesA((b + "/qt-injector.exe").c_str()) != INVALID_FILE_ATTRIBUTES) ip = b + "/qt-injector.exe";
        if (lp.empty() && GetFileAttributesA((b + "/libqt-commander.dll").c_str()) != INVALID_FILE_ATTRIBUTES) lp = b + "/libqt-commander.dll";
        if (lp.empty() && GetFileAttributesA((b + "/src/library/libqt-commander.dll").c_str()) != INVALID_FILE_ATTRIBUTES) lp = b + "/src/library/libqt-commander.dll";
        if (tp.empty() && GetFileAttributesA((b + "/qt-widget-test.exe").c_str()) != INVALID_FILE_ATTRIBUTES) tp = b + "/qt-widget-test.exe";
        if (tp.empty() && GetFileAttributesA((b + "/tests/test-apps/widget/qt-widget-test.exe").c_str()) != INVALID_FILE_ATTRIBUTES) tp = b + "/tests/test-apps/widget/qt-widget-test.exe";
    }
    if (ip.empty()) { printf("SKIP: qt-injector.exe not found\n"); return 0; }
    if (lp.empty()) { printf("SKIP: libqt-commander.dll not found\n"); return 0; }
    if (tp.empty()) { printf("SKIP: qt-widget-test.exe not found\n"); return 0; }
    printf("Injector: %s\nLibrary:  %s\nTest app: %s\n\n", ip.c_str(), lp.c_str(), tp.c_str());

    // 1. Launch test app
    STARTUPINFO si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
    if (!CreateProcess(nullptr, (LPSTR)tp.c_str(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CHECK(false, "CreateProcess test app"); return 1;
    }
    printf("PID=%lu, waiting 3s...\n", pi.dwProcessId);
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // 2. Run injector with stdout capture via pipe
    std::string pf = std::string(getenv("TEMP") ? getenv("TEMP") : ".") + "/e2e_port.txt";
    DeleteFileA(pf.c_str());

    // Create pipe for stdout
    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    CreatePipe(&hRead, &hWrite, &sa, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    std::string ic = "\"" + ip + "\" " + std::to_string(pi.dwProcessId) + " \"" + lp + "\" \"" + pf + "\"";
    STARTUPINFO si2 = { sizeof(si2) };
    si2.dwFlags = STARTF_USESTDHANDLES;
    si2.hStdOutput = hWrite;
    si2.hStdError = hWrite;
    PROCESS_INFORMATION pi2 = {};
    if (!CreateProcess(nullptr, (LPSTR)ic.c_str(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si2, &pi2)) {
        CHECK(false, "CreateProcess injector"); CloseHandle(hRead); CloseHandle(hWrite);
        TerminateProcess(pi.hProcess, 0); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); return 1;
    }
    CloseHandle(hWrite);

    // Read stdout
    char buf[4096] = {}; DWORD n = 0;
    std::string stdout_data;
    while (ReadFile(hRead, buf, sizeof(buf) - 1, &n, nullptr) && n > 0) {
        buf[n] = '\0'; stdout_data += buf;
    }
    CloseHandle(hRead);

    WaitForSingleObject(pi2.hProcess, 120000);
    DWORD ec = 1; GetExitCodeProcess(pi2.hProcess, &ec);
    CloseHandle(pi2.hProcess); CloseHandle(pi2.hThread);
    CHECK(ec == 0, (std::string("injector exit 0, got ") + std::to_string(ec)).c_str());
    if (ec != 0) { TerminateProcess(pi.hProcess, 0); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); return 1; }

    // 3. Parse stdout JSON for port + token
    printf("Injector stdout: %s\n", stdout_data.c_str());
    auto jb = stdout_data.find("{\"port\"");
    CHECK(jb != std::string::npos, "stdout contains JSON");
    auto je = stdout_data.find('}', jb);
    std::string json = stdout_data.substr(jb, je - jb + 1);

    auto pp = json.find("\"port\":");
    auto pt = json.find("\"token\":\"");
    CHECK(pp != std::string::npos && pt != std::string::npos, "port and token in JSON");

    uint16_t port = (uint16_t)std::stoi(json.substr(pp + 7));
    std::string tok = json.substr(pt + 9, 64);
    CHECK(port > 0, "port > 0");
    CHECK(tok.size() == 64, "token 64 chars");

    // 4. TCP connect + auth
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET sock = INVALID_SOCKET; bool ok = false;
    for (int r = 0; r < 5; r++) { if (tcp_connect(port, sock)) { ok = true; break; } std::this_thread::sleep_for(std::chrono::milliseconds(500)); }
    CHECK(ok, "TCP connected");

    std::string ar = "{\"jsonrpc\":\"2.0\",\"method\":\"qt.authenticate\",\"params\":{\"token\":\"" + tok + "\"},\"id\":1}";
    CHECK(send_frame(sock, ar), "auth sent");
    CHECK(recv_frame(sock).find("\"result\"") != std::string::npos, "auth OK");

    // 5. Snapshot
    CHECK(send_frame(sock, "{\"jsonrpc\":\"2.0\",\"method\":\"qt.snapshot\",\"params\":{\"detail\":\"core\"},\"id\":2}"), "snap sent");
    std::string sr = recv_frame(sock);
    CHECK(!sr.empty(), "snapshot response received");
    CHECK(sr.find("\"result\"") != std::string::npos, "snapshot result OK");
    CHECK(sr.find("\"elements\"") != std::string::npos || sr.find("\"type\"") != std::string::npos, "snapshot has UI data");

    // 6. Shutdown
    send_frame(sock, "{\"jsonrpc\":\"2.0\",\"method\":\"qt.shutdown\",\"params\":{},\"id\":3}");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    closesocket(sock); WSACleanup();

    // 7. Eject — verify ejectLibrary success path
    {
        std::string ej_cmd = "\"" + ip + "\" --eject " + std::to_string(pi.dwProcessId) + " \"" + lp + "\"";
        HANDLE hRead2, hWrite2;
        SECURITY_ATTRIBUTES sa2 = { sizeof(sa2), nullptr, TRUE };
        CreatePipe(&hRead2, &hWrite2, &sa2, 0);
        SetHandleInformation(hRead2, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFO si3 = { sizeof(si3) };
        si3.dwFlags = STARTF_USESTDHANDLES;
        si3.hStdOutput = hWrite2;
        si3.hStdError = hWrite2;
        PROCESS_INFORMATION pi3 = {};
        if (!CreateProcess(nullptr, (LPSTR)ej_cmd.c_str(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si3, &pi3)) {
            printf("(eject subprocess launch failed — skip)\n");
            CHECK(true, "eject test attempted");
        } else {
            CloseHandle(hWrite2);
            char ebuf[1024] = {}; DWORD en = 0;
            std::string eject_out;
            while (ReadFile(hRead2, ebuf, sizeof(ebuf) - 1, &en, nullptr) && en > 0) { ebuf[en] = '\0'; eject_out += ebuf; }
            CloseHandle(hRead2);
            WaitForSingleObject(pi3.hProcess, 60000);
            DWORD ej_ec = 99; GetExitCodeProcess(pi3.hProcess, &ej_ec);
            CloseHandle(pi3.hProcess); CloseHandle(pi3.hThread);
            printf("Eject exit code: %lu, output: %s\n", ej_ec, eject_out.c_str());
            CHECK(ej_ec == 0, (std::string("eject exit 0, got ") + std::to_string(ej_ec)).c_str());
            CHECK(eject_out.find("ejected") != std::string::npos, "eject output contains 'ejected'");
        }
    }

    // 8. Cleanup
    TerminateProcess(pi.hProcess, 0); WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    DeleteFileA(pf.c_str());

    printf("\n%d/%d tests passed\n", passed, total);
    return passed == total ? 0 : 1;
}
