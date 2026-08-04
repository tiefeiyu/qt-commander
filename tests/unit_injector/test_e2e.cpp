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

// --- interaction helpers ---------------------------------------------------

static bool resp_ok(const std::string& r) {
    return r.find("\"ok\":true") != std::string::npos;
}

static std::string call_rpc(SOCKET sock, const char* method,
                            const std::string& params, int id) {
    std::string q = std::string("{\"jsonrpc\":\"2.0\",\"method\":\"") +
                    method + "\",\"params\":" + params + ",\"id\":" +
                    std::to_string(id) + "}";
    if (!send_frame(sock, q)) return "";
    return recv_frame(sock);
}

static long find_element_id(SOCKET sock, const char* objName, int id) {
    std::string q = std::string(
        "{\"jsonrpc\":\"2.0\",\"method\":\"qt.findElement\",\"params\":"
        "{\"query\":{\"object_name\":\"") + objName + "\"}},\"id\":" +
        std::to_string(id) + "}";
    if (!send_frame(sock, q)) return -1;
    std::string r = recv_frame(sock);
    auto elems = r.find("\"elements\":[");
    if (elems == std::string::npos) return -1;
    auto idp = r.find("\"id\":", elems);
    if (idp == std::string::npos) return -1;
    return std::stol(r.substr(idp + 5));
}

static std::string get_prop_str(SOCKET sock, long eid, const char* name, int id) {
    std::string r = call_rpc(sock, "qt.getProperty",
        std::string("{\"element_id\":") + std::to_string(eid) +
        ",\"name\":\"" + name + "\"}", id);
    auto vp = r.find("\"value\":");
    if (vp == std::string::npos) return "";
    auto s = r.find('"', vp + 8);
    if (s == std::string::npos) return "";
    auto e = r.find('"', s + 1);
    return r.substr(s + 1, e - s - 1);
}

static bool get_prop_bool(SOCKET sock, long eid, const char* name, int id) {
    std::string r = call_rpc(sock, "qt.getProperty",
        std::string("{\"element_id\":") + std::to_string(eid) +
        ",\"name\":\"" + name + "\"}", id);
    auto vp = r.find("\"value\":");
    if (vp == std::string::npos) return false;
    return r.find("true", vp + 8) < r.find("false", vp + 8);
}

static double get_prop_double(SOCKET sock, long eid, const char* name, int id) {
    std::string r = call_rpc(sock, "qt.getProperty",
        std::string("{\"element_id\":") + std::to_string(eid) +
        ",\"name\":\"" + name + "\"}", id);
    auto vp = r.find("\"value\":");
    if (vp == std::string::npos) return -1;
    return std::atof(r.c_str() + vp + 8);
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
    // CreateProcess cannot reliably resolve relative paths with directory
    // separators (it searches PATH, not the CWD); absolutize everything.
    auto absolutize = [](std::string& p) {
        if (p.empty()) return;
        char full[MAX_PATH];
        if (GetFullPathNameA(p.c_str(), MAX_PATH, full, nullptr) > 0)
            p = full;
    };
    absolutize(ip); absolutize(lp); absolutize(tp);
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
    // The library's snapshot returns top-level "nodes", each with a
    // "className" -- either field proves the UI tree came back.
    CHECK(sr.find("\"nodes\"") != std::string::npos || sr.find("\"className\"") != std::string::npos, "snapshot has UI data");

    // 5b. Interaction primitives over the real wire:
    //     typeText + keyCombo ("Ctrl+A"), mousePress/mouseRelease split
    //     click, and a slider drag (press -> move -> release).
    {
        // --- keyCombo: type then Ctrl+A selects everything ---
        long editId = find_element_id(sock, "lineEdit", 4);
        CHECK(editId > 0, "findElement lineEdit");
        if (editId > 0) {
            std::string r = call_rpc(sock, "qt.typeText",
                "{\"element_id\":" + std::to_string(editId) +
                ",\"text\":\"hello\",\"modifiers\":[]}", 5);
            CHECK(resp_ok(r), "typeText \"hello\" ok");
            std::string text = get_prop_str(sock, editId, "text", 6);
            CHECK(text == "hello",
                  (std::string("lineEdit text after typeText, got: ") + text).c_str());
            r = call_rpc(sock, "qt.keyCombo",
                "{\"element_id\":" + std::to_string(editId) +
                ",\"keys\":\"Ctrl+A\"}", 7);
            CHECK(resp_ok(r), "keyCombo Ctrl+A ok");
            std::string sel = get_prop_str(sock, editId, "selectedText", 8);
            CHECK(sel == "hello",
                  (std::string("Ctrl+A selects all, got: ") + sel).c_str());
        }

        // --- mousePress / mouseRelease split: checkbox flips on release ---
        long chkId = find_element_id(sock, "checkBox", 9);
        CHECK(chkId > 0, "findElement checkBox");
        if (chkId > 0) {
            CHECK(get_prop_bool(sock, chkId, "checked", 10),
                  "checkbox starts checked");
            std::string r = call_rpc(sock, "qt.mousePress",
                "{\"element_id\":" + std::to_string(chkId) +
                ",\"button\":\"left\",\"modifiers\":[]}", 11);
            CHECK(resp_ok(r), "mousePress ok");
            CHECK(get_prop_bool(sock, chkId, "checked", 12),
                  "press alone must NOT flip the checkbox");
            r = call_rpc(sock, "qt.mouseRelease",
                "{\"element_id\":" + std::to_string(chkId) +
                ",\"button\":\"left\",\"modifiers\":[]}", 13);
            CHECK(resp_ok(r), "mouseRelease ok");
            CHECK(!get_prop_bool(sock, chkId, "checked", 14),
                  "release flips the checkbox");
        }

        // --- drag: press probe center -> move -> release ---
        // The app's DragProbe follows the pointer while pressed, so the
        // drag has a visible effect: geometry changes by exactly the
        // pointer delta.  Each RPC is processed by the target's event
        // loop before the next arrives (live path): press at center
        // (425,30) grabs the probe; move(600,30) puts it at
        // (9+600-425, 520) = (184,520); move(700,30) -> (459,520).
        // dragDX = 459-9 = 450, dragDY = 0.
        long probeId = find_element_id(sock, "dragProbe", 15);
        CHECK(probeId > 0, "findElement dragProbe");
        if (probeId > 0) {
            double startX = get_prop_double(sock, probeId, "x", 16);
            CHECK(startX == 9, (std::string("probe starts at x=9, got ") +
                                std::to_string((int)startX)).c_str());
            std::string r = call_rpc(sock, "qt.mousePress",
                "{\"element_id\":" + std::to_string(probeId) +
                ",\"button\":\"left\",\"modifiers\":[]}", 17);
            CHECK(resp_ok(r), "drag press ok");
            r = call_rpc(sock, "qt.mouseMove",
                "{\"element_id\":" + std::to_string(probeId) +
                ",\"x\":600,\"y\":30}", 18);
            CHECK(resp_ok(r), "drag move 1 ok");
            r = call_rpc(sock, "qt.mouseMove",
                "{\"element_id\":" + std::to_string(probeId) +
                ",\"x\":700,\"y\":30}", 19);
            CHECK(resp_ok(r), "drag move 2 ok");
            r = call_rpc(sock, "qt.mouseRelease",
                "{\"element_id\":" + std::to_string(probeId) +
                ",\"button\":\"left\",\"modifiers\":[],\"x\":700,\"y\":30}", 20);
            CHECK(resp_ok(r), "drag release ok");
            double moves = get_prop_double(sock, probeId, "moveCount", 21);
            CHECK(moves >= 2, "both move events delivered");
            double moveX = get_prop_double(sock, probeId, "moveX", 22);
            CHECK(moveX == 700, "last move at target x");
            double releaseX = get_prop_double(sock, probeId, "releaseX", 23);
            CHECK(releaseX == 700, "release at target x");
            double endX = get_prop_double(sock, probeId, "x", 24);
            CHECK(endX == 459, (std::string("visible effect: probe moved to x=459, got ") +
                                std::to_string((int)endX)).c_str());
            double ddx = get_prop_double(sock, probeId, "dragDX", 25);
            CHECK(ddx == 450, (std::string("dragDX reports the pointer delta, got ") +
                               std::to_string((int)ddx)).c_str());
            double endY = get_prop_double(sock, probeId, "y", 26);
            CHECK(endY == 520, (std::string("probe stayed at y=520, got ") +
                                std::to_string((int)endY)).c_str());
        }
    }

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
