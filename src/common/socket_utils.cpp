#include "socket_utils.h"

#include <cstring>
#include <mutex>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Platform headers
// ---------------------------------------------------------------------------
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#else
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#endif

// ---------------------------------------------------------------------------
// RAII wrapper for WSAStartup (Windows only), one-shot initializer
// ---------------------------------------------------------------------------
#ifdef _WIN32

struct WsaGuard {
    WsaGuard() noexcept {
        WSADATA wsaData{};
        int rc = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (rc != 0) {
            ok_ = false;
        }
    }
    ~WsaGuard() noexcept {
        if (ok_) {
            WSACleanup();
        }
    }
    bool ok() const noexcept { return ok_; }

private:
    bool ok_ = true;
};

static WsaGuard& wsa_guard() noexcept {
    static WsaGuard guard;
    return guard;
}

// Convert Windows error-code to errno-style positive number for uniform logging.
static int socket_last_error() noexcept { return static_cast<int>(WSAGetLastError()); }

// Check if errno indicates a transient (interrupted) condition.
static bool is_transient_error(int err) noexcept {
    return err == WSAEINTR || err == WSAEWOULDBLOCK;
}

#else // POSIX

static int socket_last_error() noexcept { return errno; }

static bool is_transient_error(int err) noexcept {
    return err == EINTR;
}

#endif

// ---------------------------------------------------------------------------
// Helper: convert signed timeout-milliseconds to POSIX timeval / DWORD
// ---------------------------------------------------------------------------
#ifdef _WIN32
static DWORD timeout_ms_to_win(int ms) noexcept {
    return ms < 0 ? 0 : static_cast<DWORD>(ms);
}
#else
static struct timeval timeout_ms_to_posix(int ms) noexcept {
    if (ms < 0) {
        ms = 0;
    }
    struct timeval tv{};
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return tv;
}
#endif

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool socket_init() {
#ifdef _WIN32
    return wsa_guard().ok();
#else
    // POSIX: no global init needed.
    return true;
#endif
}

void socket_cleanup() {
    // On Windows the WsaGuard destructor runs at static-destruction time.
    // Nothing else to do.  If explicit early cleanup is desired the user
    // can call this; the guard will not double-clean because WSACleanup
    // is safe to call once.  For simplicity we do nothing here so the
    // guard's lifetime remains tied to static lifetime.
#ifdef _WIN32
    // This is intentionally a no-op; the RAII guard handles cleanup at exit.
    (void)wsa_guard();
#else
    // No-op on POSIX.
#endif
}

// ---------------------------------------------------------------------------
// tcp_listen_loopback
// ---------------------------------------------------------------------------
socket_t tcp_listen_loopback(uint16_t& port) {
#ifdef _WIN32
    SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) {
        return INVALID_SOCK;
    }
#else
    int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) {
        return INVALID_SOCK;
    }
#endif

    // Allow immediate reuse of the address (avoids TIME_WAIT issues on restart).
    int optval = 1;
#ifdef _WIN32
    setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&optval), sizeof(optval));
#else
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
#endif

    struct sockaddr_in addr{};
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1
    addr.sin_port        = 0;                      // OS picks port

#ifdef _WIN32
    int rc = ::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), static_cast<int>(sizeof(addr)));
#else
    int rc = ::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
#endif
    if (rc != 0) {
        tcp_close(static_cast<socket_t>(fd));
        return INVALID_SOCK;
    }

#ifdef _WIN32
    rc = ::listen(fd, SOMAXCONN);
#else
    rc = ::listen(fd, SOMAXCONN);
#endif
    if (rc != 0) {
        tcp_close(static_cast<socket_t>(fd));
        return INVALID_SOCK;
    }

    // Retrieve the port assigned by the kernel.
    struct sockaddr_in bound{};
#ifdef _WIN32
    int bound_len = static_cast<int>(sizeof(bound));
#else
    socklen_t bound_len = sizeof(bound);
#endif
    rc = ::getsockname(fd, reinterpret_cast<struct sockaddr*>(&bound), &bound_len);
    if (rc != 0) {
        tcp_close(static_cast<socket_t>(fd));
        return INVALID_SOCK;
    }

    port = ntohs(bound.sin_port);
    return static_cast<socket_t>(fd);
}

// ---------------------------------------------------------------------------
// tcp_accept
// ---------------------------------------------------------------------------
socket_t tcp_accept(socket_t listen_fd) {
#ifdef _WIN32
    SOCKET fd = ::accept(static_cast<SOCKET>(listen_fd), nullptr, nullptr);
    return (fd == INVALID_SOCKET) ? INVALID_SOCK : static_cast<socket_t>(fd);
#else
    int fd = ::accept(static_cast<int>(listen_fd), nullptr, nullptr);
    return (fd < 0) ? INVALID_SOCK : static_cast<socket_t>(fd);
#endif
}

// ---------------------------------------------------------------------------
// tcp_connect_loopback
// ---------------------------------------------------------------------------
socket_t tcp_connect_loopback(uint16_t port) {
#ifdef _WIN32
    SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) {
        return INVALID_SOCK;
    }
#else
    int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) {
        return INVALID_SOCK;
    }
#endif

    struct sockaddr_in addr{};
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(port);

#ifdef _WIN32
    int rc = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), static_cast<int>(sizeof(addr)));
    if (rc != 0) {
        closesocket(fd);
        return INVALID_SOCK;
    }
#else
    int rc = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc != 0) {
        ::close(fd);
        return INVALID_SOCK;
    }
#endif

    return static_cast<socket_t>(fd);
}

// ---------------------------------------------------------------------------
// tcp_close
// ---------------------------------------------------------------------------
void tcp_close(socket_t fd) {
    if (fd == INVALID_SOCK) {
        return;
    }
#ifdef _WIN32
    ::closesocket(static_cast<SOCKET>(fd));
#else
    ::close(static_cast<int>(fd));
#endif
}

// ---------------------------------------------------------------------------
// tcp_send_all
// ---------------------------------------------------------------------------
bool tcp_send_all(socket_t fd, const void* data, size_t len) {
    auto* buf = static_cast<const char*>(data);
    while (len > 0) {
#ifdef _WIN32
        int sent = ::send(static_cast<SOCKET>(fd), buf, static_cast<int>(len > INT_MAX ? INT_MAX : len), 0);
        if (sent == SOCKET_ERROR) {
            int err = socket_last_error();
            if (err == WSAEINTR) {
                continue; // retry
            }
            return false;
        }
#else
        ssize_t sent = ::send(static_cast<int>(fd), buf, len, 0);
        if (sent < 0) {
            int err = socket_last_error();
            if (err == EINTR) {
                continue; // retry
            }
            return false;
        }
#endif
        buf += sent;
        len -= static_cast<size_t>(sent);
    }
    return true;
}

// ---------------------------------------------------------------------------
// tcp_recv_all
// ---------------------------------------------------------------------------
bool tcp_recv_all(socket_t fd, void* buf, size_t len) {
    auto* ptr = static_cast<char*>(buf);
    while (len > 0) {
#ifdef _WIN32
        int n = ::recv(static_cast<SOCKET>(fd), ptr, static_cast<int>(len > INT_MAX ? INT_MAX : len), 0);
        if (n == SOCKET_ERROR) {
            int err = socket_last_error();
            if (err == WSAEINTR) {
                continue;
            }
            return false;
        }
#else
        ssize_t n = ::recv(static_cast<int>(fd), ptr, len, 0);
        if (n < 0) {
            int err = socket_last_error();
            if (err == EINTR) {
                continue;
            }
            return false;
        }
#endif
        if (n == 0) {
            // Peer closed connection (graceful shutdown).
            return false;
        }
        ptr += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

// ---------------------------------------------------------------------------
// tcp_set_recv_timeout
// ---------------------------------------------------------------------------
void tcp_set_recv_timeout(socket_t fd, int timeout_ms) {
#ifdef _WIN32
    DWORD tv = timeout_ms_to_win(timeout_ms);
    ::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    struct timeval tv = timeout_ms_to_posix(timeout_ms);
    ::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

// ---------------------------------------------------------------------------
// tcp_set_send_timeout
// ---------------------------------------------------------------------------
void tcp_set_send_timeout(socket_t fd, int timeout_ms) {
#ifdef _WIN32
    DWORD tv = timeout_ms_to_win(timeout_ms);
    ::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    struct timeval tv = timeout_ms_to_posix(timeout_ms);
    ::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

// ---------------------------------------------------------------------------
// tcp_set_keepalive
// ---------------------------------------------------------------------------
void tcp_set_keepalive(socket_t fd, int idle_sec, int interval_sec, int count) {
    // Enable keepalive at the socket level.
    int optval = 1;
#ifdef _WIN32
    setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_KEEPALIVE,
               reinterpret_cast<const char*>(&optval), sizeof(optval));
#else
    setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval));
#endif

#ifdef _WIN32
    // Windows uses struct tcp_keepalive with WSAIoctl(SIO_KEEPALIVE_VALS).
    struct tcp_keepalive ka {};
    ka.onoff       = 1;
    ka.keepalivetime = static_cast<ULONG>(idle_sec) * 1000;    // milliseconds
    ka.keepaliveinterval = static_cast<ULONG>(interval_sec) * 1000;
    DWORD bytes_ret = 0;
    ::WSAIoctl(static_cast<SOCKET>(fd), SIO_KEEPALIVE_VALS,
               &ka, sizeof(ka), nullptr, 0, &bytes_ret, nullptr, nullptr);
#else
    // POSIX: TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT
    ::setsockopt(static_cast<int>(fd), IPPROTO_TCP, TCP_KEEPIDLE, &idle_sec, sizeof(idle_sec));
    ::setsockopt(static_cast<int>(fd), IPPROTO_TCP, TCP_KEEPINTVL, &interval_sec, sizeof(interval_sec));
    ::setsockopt(static_cast<int>(fd), IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#endif
}
