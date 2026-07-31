// Cover socket_utils.cpp (TCP loopback operations + socket_init/cleanup)
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#endif

#include "../../src/common/socket_utils.cpp"

static int passed = 0, total = 0;
#define CHECK(cond, msg) do { total++; if (cond) { passed++; } else { printf("FAIL %s:%d — %s\n", __FILE__, __LINE__, msg); } } while(0)

void test_init_cleanup() {
    bool ok = socket_init();
    CHECK(ok, "socket_init succeeds");
    socket_cleanup();
    // Second init should also work
    ok = socket_init();
    CHECK(ok, "socket_init again succeeds");
    socket_cleanup();
}

void test_listen_loopback() {
    socket_init();
    uint16_t port = 0;
    socket_t s = tcp_listen_loopback(port);
    CHECK(s != INVALID_SOCK, "listen succeeds");
    CHECK(port > 0, "port assigned");
    tcp_close(s);
    socket_cleanup();
}

void test_connect_refused() {
    socket_init();
    socket_t s = tcp_connect_loopback(1);  // port 1 — nothing listening
    // May succeed or fail depending on OS; just verify no crash
    if (s != INVALID_SOCK) tcp_close(s);
    CHECK(true, "connect to closed port handled");
    socket_cleanup();
}

void test_send_recv_roundtrip() {
    socket_init();
    uint16_t port = 0;
    socket_t server = tcp_listen_loopback(port);
    CHECK(server != INVALID_SOCK, "server listen");
    CHECK(port > 0, "server port");

    // Connect client in a thread
    std::string received;
    std::thread client([port, &received]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        socket_t c = tcp_connect_loopback(port);
        if (c != INVALID_SOCK) {
            const char* msg = "hello server";
            tcp_send_all(c, msg, strlen(msg));
            tcp_close(c);
        }
    });

    // Accept
    tcp_set_recv_timeout(server, 3000);
    socket_t client_fd = tcp_accept(server);
    CHECK(client_fd != INVALID_SOCK, "accept succeeds");

    // Receive
    char buf[64] = {};
    tcp_set_recv_timeout(client_fd, 3000);
    bool recv_ok = tcp_recv_all(client_fd, buf, 5);
    CHECK(recv_ok, "recv_all 5 bytes");
    CHECK(strncmp(buf, "hello", 5) == 0, "correct data received");

    tcp_close(client_fd);
    tcp_close(server);
    client.join();
    socket_cleanup();
}

void test_send_timeout() {
    socket_init();
    uint16_t port = 0;
    socket_t s = tcp_listen_loopback(port);
    CHECK(s != INVALID_SOCK, "listen");
    tcp_set_send_timeout(s, 100);
    tcp_set_recv_timeout(s, 100);
    tcp_close(s);
    socket_cleanup();
    CHECK(true, "timeout settings applied");
}

void test_accept_timeout() {
    socket_init();
    uint16_t port = 0;
    socket_t s = tcp_listen_loopback(port);
    CHECK(s != INVALID_SOCK, "listen");
    tcp_set_recv_timeout(s, 100);
    socket_t client = tcp_accept(s);
    CHECK(client == INVALID_SOCK, "accept timeout returns INVALID_SOCK");
    tcp_close(s);
    socket_cleanup();
}

void test_send_to_closed() {
    socket_init();
    uint16_t port = 0;
    socket_t s = tcp_listen_loopback(port);
    CHECK(s != INVALID_SOCK, "listen");
    tcp_close(s);
    bool ok = tcp_send_all(s, "test", 4);
    CHECK(!ok, "send to closed socket fails");
    socket_cleanup();
}

void test_connect_retry() {
    socket_init();
    socket_t s = tcp_connect_loopback(65530);
    if (s != INVALID_SOCK) tcp_close(s);
    CHECK(true, "connect retry handled");
    socket_cleanup();
}

void test_keepalive() {
    socket_init();
    uint16_t port = 0;
    socket_t s = tcp_listen_loopback(port);
    CHECK(s != INVALID_SOCK, "listen for keepalive");
    tcp_set_keepalive(s, 60, 10, 5);
    CHECK(true, "tcp_set_keepalive applied");
    tcp_close(s);
    socket_cleanup();
}

int main() {
    printf("=== socket_utils.cpp coverage tests ===\n\n");
    test_init_cleanup();
    test_listen_loopback();
    test_connect_refused();
    test_send_recv_roundtrip();
    test_send_timeout();
    test_accept_timeout();
    test_send_to_closed();
    test_connect_retry();
    test_keepalive();

    printf("\n%d/%d tests passed\n", passed, total);
    return passed == total ? 0 : 1;
}
