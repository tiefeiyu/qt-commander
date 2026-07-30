#include <iostream>
#include <cstring>
#include <string>
#include "common/socket_utils.h"

static int passed = 0, failed = 0;

#define TEST(name) do { std::cout << "  " << name << "... "; } while(0)
#define PASS() do { std::cout << "PASS\n"; passed++; } while(0)
#define FAIL(msg) do { std::cout << "FAIL: " << msg << "\n"; failed++; } while(0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

// ---------------------------------------------------------------------------
// 1. socket_init / socket_cleanup lifecycle
// ---------------------------------------------------------------------------
static void test_init_cleanup() {
    TEST("socket_init returns true");
    bool ok = socket_init();
    CHECK(ok, "socket_init should succeed on first call");
    PASS();

    TEST("socket_cleanup callable (no crash)");
    socket_cleanup();
    PASS();

    TEST("re-init after cleanup");
    ok = socket_init();
    CHECK(ok, "second socket_init should also succeed");
    PASS();
}

// ---------------------------------------------------------------------------
// 2. tcp_listen_loopback returns valid socket and non-zero port
// ---------------------------------------------------------------------------
static void test_listen_loopback() {
    uint16_t port = 0;
    socket_t fd = tcp_listen_loopback(port);
    CHECK(fd != INVALID_SOCK, "listen_loopback should return valid socket");
    CHECK(port != 0, "port should be non-zero (OS-assigned)");
    tcp_close(fd);
    PASS();
}

// ---------------------------------------------------------------------------
// 3. tcp_connect_loopback successfully connects to listening socket
// ---------------------------------------------------------------------------
static void test_connect_loopback() {
    uint16_t port = 0;
    socket_t listen_fd = tcp_listen_loopback(port);
    CHECK(listen_fd != INVALID_SOCK, "listen failed");

    socket_t client_fd = tcp_connect_loopback(port);
    CHECK(client_fd != INVALID_SOCK, "connect_loopback should succeed");

    // Accept so the connection completes.
    socket_t server_fd = tcp_accept(listen_fd);
    CHECK(server_fd != INVALID_SOCK, "accept should complete the connection");

    tcp_close(server_fd);
    tcp_close(client_fd);
    tcp_close(listen_fd);
    PASS();
}

// ---------------------------------------------------------------------------
// 4. Full round-trip: listen -> connect -> accept -> send_all -> recv_all -> close
// ---------------------------------------------------------------------------
static void test_send_recv_roundtrip() {
    uint16_t port = 0;
    socket_t listen_fd = tcp_listen_loopback(port);
    CHECK(listen_fd != INVALID_SOCK, "listen failed");

    socket_t client_fd = tcp_connect_loopback(port);
    CHECK(client_fd != INVALID_SOCK, "connect failed");

    socket_t server_fd = tcp_accept(listen_fd);
    CHECK(server_fd != INVALID_SOCK, "accept failed");

    const char* test_data = "Hello TCP Roundtrip!";
    size_t data_len = std::strlen(test_data);

    // Client sends, server receives.
    bool ok = tcp_send_all(client_fd, test_data, data_len);
    CHECK(ok, "send_all (client) should succeed");

    char buf[128] = {};
    ok = tcp_recv_all(server_fd, buf, data_len);
    CHECK(ok, "recv_all (server) should succeed");
    CHECK(std::strcmp(buf, test_data) == 0, "received data should match sent data");

    // Bidirectional: server sends back, client receives.
    const char* reply = "ACK";
    size_t reply_len = std::strlen(reply);
    ok = tcp_send_all(server_fd, reply, reply_len);
    CHECK(ok, "send_all (server) should succeed");

    char reply_buf[16] = {};
    ok = tcp_recv_all(client_fd, reply_buf, reply_len);
    CHECK(ok, "recv_all (client) should succeed");
    CHECK(std::strcmp(reply_buf, reply) == 0, "reply data should match");

    tcp_close(server_fd);
    tcp_close(client_fd);
    tcp_close(listen_fd);
    PASS();
}

// ---------------------------------------------------------------------------
// 5. tcp_accept returns a valid, usable socket
// ---------------------------------------------------------------------------
static void test_accept_valid() {
    uint16_t port = 0;
    socket_t listen_fd = tcp_listen_loopback(port);
    CHECK(listen_fd != INVALID_SOCK, "listen failed");

    socket_t client_fd = tcp_connect_loopback(port);
    CHECK(client_fd != INVALID_SOCK, "connect failed");

    socket_t server_fd = tcp_accept(listen_fd);
    CHECK(server_fd != INVALID_SOCK, "accept should return valid socket");

    // Verify accepted socket is usable.
    const char* ping = "ping";
    bool ok = tcp_send_all(client_fd, ping, 4);
    CHECK(ok, "send via accepted socket chain should succeed");

    char pong[4] = {};
    ok = tcp_recv_all(server_fd, pong, 4);
    CHECK(ok, "recv on accepted socket should succeed");

    tcp_close(server_fd);
    tcp_close(client_fd);
    tcp_close(listen_fd);
    PASS();
}

// ---------------------------------------------------------------------------
// 6. Reject connection to invalid port (port 1, privileged / unlikely to be open)
// ---------------------------------------------------------------------------
static void test_invalid_port() {
    socket_t fd = tcp_connect_loopback(1);
    CHECK(fd == INVALID_SOCK, "connect to port 1 should fail");
    PASS();
}

// ---------------------------------------------------------------------------
// 7. tcp_set_recv_timeout -- verify timeout on idle socket
// ---------------------------------------------------------------------------
static void test_recv_timeout() {
    uint16_t port = 0;
    socket_t listen_fd = tcp_listen_loopback(port);
    CHECK(listen_fd != INVALID_SOCK, "listen failed");

    socket_t client_fd = tcp_connect_loopback(port);
    CHECK(client_fd != INVALID_SOCK, "connect failed");

    socket_t server_fd = tcp_accept(listen_fd);
    CHECK(server_fd != INVALID_SOCK, "accept failed");

    // Set a short receive timeout -- no data will be sent from the client.
    tcp_set_recv_timeout(server_fd, 100);

    char buf[1];
    bool ok = tcp_recv_all(server_fd, buf, 1);
    CHECK(!ok, "recv_all should fail/timeout when no data is sent");

    tcp_close(server_fd);
    tcp_close(client_fd);
    tcp_close(listen_fd);
    PASS();
}

// ---------------------------------------------------------------------------
// 8. tcp_set_keepalive -- verify it does not crash
// ---------------------------------------------------------------------------
static void test_keepalive() {
    uint16_t port = 0;
    socket_t fd = tcp_listen_loopback(port);
    CHECK(fd != INVALID_SOCK, "listen failed");

    // Setting keepalive should not crash on any platform.
    tcp_set_keepalive(fd, 10, 5, 3);
    PASS();

    tcp_close(fd);
}

// ===========================================================================

int main() {
    if (!socket_init()) {
        std::cerr << "FATAL: socket_init() failed\n";
        return 1;
    }

    std::cout << "test_socket_utils\n";

    test_init_cleanup();     // calls init/cleanup internally
    test_listen_loopback();
    test_connect_loopback();
    test_send_recv_roundtrip();
    test_accept_valid();
    test_invalid_port();
    test_recv_timeout();
    test_keepalive();

    socket_cleanup();

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed > 0 ? 1 : 0;
}
