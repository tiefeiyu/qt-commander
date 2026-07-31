#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#endif

#include <cstdint>
#include <cstddef>

// Cross-platform TCP socket abstraction for loopback (127.0.0.1) only.
// Windows uses WinSock2; POSIX uses standard BSD sockets.

using socket_t = uintptr_t;
static const socket_t INVALID_SOCK = ~(socket_t)0;

// Initialize socket subsystem. Must be called once before any socket operations.
// On Windows: calls WSAStartup. On POSIX: no-op. Thread-safe (call once).
bool socket_init();

// Cleanup socket subsystem. Call once at process exit.
void socket_cleanup();

// Bind and listen on 127.0.0.1:0 (OS picks random port). Returns actual port in 'port'.
// Returns INVALID_SOCK on error.
socket_t tcp_listen_loopback(uint16_t& port);

// Accept one connection on a listening socket. Returns INVALID_SOCK on error.
socket_t tcp_accept(socket_t listen_fd);

// Connect to 127.0.0.1:port. Returns INVALID_SOCK on error.
socket_t tcp_connect_loopback(uint16_t port);

// Close a socket.
void tcp_close(socket_t fd);

// Send all bytes; loops until all sent or error. Returns false on error.
bool tcp_send_all(socket_t fd, const void* data, size_t len);

// Receive exactly len bytes; loops until all received or error. Returns false on error/disconnect.
bool tcp_recv_all(socket_t fd, void* buf, size_t len);

// Set receive timeout in milliseconds.
void tcp_set_recv_timeout(socket_t fd, int timeout_ms);

// Set send timeout in milliseconds.
void tcp_set_send_timeout(socket_t fd, int timeout_ms);

// Set keepalive with given interval seconds and idle time.
void tcp_set_keepalive(socket_t fd, int idle_sec, int interval_sec, int count);
