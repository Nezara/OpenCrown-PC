// Socket compat for the net layer: one source set over Winsock (Windows dev host) and BSD sockets (Android). Only the
// differences the net code meets: socket handle type, close, last error / would-block / EINTR, select's nfds, the
// const char* buffer casts and int lengths of Winsock, MSG_NOSIGNAL (Windows has no SIGPIPE), receive timeouts as
// DWORD ms vs timeval, ICMP port-unreachable resets on UDP, and process-wide setup (WSAStartup / SIGPIPE ignored).
#pragma once
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX  // std::min / std::max over windows.h's macros
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dcp::net::sock {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalid = INVALID_SOCKET;
constexpr int kNoSignal = 0;  // no SIGPIPE on Windows
using IoLen = int;            // Winsock buffer lengths and results
#else
using Socket = int;
constexpr Socket kInvalid = -1;
constexpr int kNoSignal = MSG_NOSIGNAL;  // a peer that hung up gives EPIPE, not a kill
using IoLen = size_t;
#endif

// Process-wide setup, once (idempotent, thread-safe): WSAStartup on Windows, SIGPIPE ignored on POSIX (send paths
// that cannot pass MSG_NOSIGNAL, e.g. miniupnpc). false only when Winsock cannot start.
bool startup();

inline int close(Socket s) {
#ifdef _WIN32
    return ::closesocket(s);
#else
    return ::close(s);
#endif
}

inline int last_error() {
#ifdef _WIN32
    return ::WSAGetLastError();
#else
    return errno;
#endif
}
inline bool would_block(int e) {
#ifdef _WIN32
    return e == WSAEWOULDBLOCK;
#else
    return e == EAGAIN || e == EWOULDBLOCK;
#endif
}
inline bool interrupted(int e) {
#ifdef _WIN32
    return e == WSAEINTR;
#else
    return e == EINTR;
#endif
}

// select()'s first argument for a set whose highest socket is `max_s` (ignored by Winsock).
inline int nfds(Socket max_s) {
#ifdef _WIN32
    (void)max_s;
    return 0;
#else
    return max_s + 1;
#endif
}

// Whether an fd_set can take one more socket: Winsock sets hold FD_SETSIZE sockets of any value (`count` already in
// it), POSIX sets hold descriptors below FD_SETSIZE.
inline bool fits_fd_set(Socket s, size_t count) {
#ifdef _WIN32
    (void)s;
    return count < FD_SETSIZE;
#else
    (void)count;
    return s >= 0 && s < FD_SETSIZE;
#endif
}

// New socket (close-on-exec on POSIX). kInvalid on failure.
inline Socket open(int type, int proto) {
#ifdef _WIN32
    return ::socket(AF_INET, type, proto);
#else
    return ::socket(AF_INET, type | SOCK_CLOEXEC, proto);
#endif
}

inline Socket accept(Socket ls, sockaddr_in* from) {
    socklen_t len = sizeof *from;
#ifdef _WIN32
    return ::accept(ls, reinterpret_cast<sockaddr*>(from), &len);
#else
    return ::accept4(ls, reinterpret_cast<sockaddr*>(from), &len, SOCK_CLOEXEC);
#endif
}

inline bool set_nonblocking(Socket s, bool on) {
#ifdef _WIN32
    u_long nb = on ? 1 : 0;
    return ::ioctlsocket(s, FIONBIO, &nb) == 0;
#else
    int fl = ::fcntl(s, F_GETFL);
    return fl >= 0 && ::fcntl(s, F_SETFL, on ? fl | O_NONBLOCK : fl & ~O_NONBLOCK) == 0;
#endif
}

// SO_RCVTIMEO + SO_SNDTIMEO (0 = block without limit).
inline void set_timeouts(Socket s, int ms) {
#ifdef _WIN32
    DWORD t = DWORD(ms);
#else
    timeval t{ms / 1000, (ms % 1000) * 1000};
#endif
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
}

inline void set_int_opt(Socket s, int level, int name, int v) {
    ::setsockopt(s, level, name, reinterpret_cast<const char*>(&v), sizeof v);
}
inline int get_error_opt(Socket s) {  // SO_ERROR: a failed non-blocking connect's error, 0 if none
    int e = 0;
    socklen_t len = sizeof e;
    ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&e), &len);
    return e;
}

// A UDP socket must not fail recvfrom after sending to a port nobody listens on yet (Windows reports the ICMP
// port-unreachable as WSAECONNRESET on the next recvfrom; an unconnected POSIX UDP socket never sees it).
inline void udp_ignore_unreachable(Socket s) {
#ifdef _WIN32
    BOOL no_reset = FALSE;
    DWORD ret = 0;
    ::WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /*SIO_UDP_CONNRESET*/, &no_reset, sizeof no_reset, nullptr, 0, &ret, nullptr,
               nullptr);
#else
    (void)s;
#endif
}

// send / recv / sendto / recvfrom with the platform's buffer types; results as int (-1 = error, see last_error()).
inline int send(Socket s, const void* p, size_t n) {
    return int(::send(s, static_cast<const char*>(p), IoLen(n), kNoSignal));
}
inline int recv(Socket s, void* p, size_t n) { return int(::recv(s, static_cast<char*>(p), IoLen(n), 0)); }
inline int send_to(Socket s, const void* p, size_t n, const sockaddr_in& to) {
    return int(::sendto(s, static_cast<const char*>(p), IoLen(n), kNoSignal, reinterpret_cast<const sockaddr*>(&to),
                        sizeof to));
}
inline int recv_from(Socket s, void* p, size_t n, sockaddr_in* from) {
    socklen_t fl = sizeof *from;
    return int(::recvfrom(s, static_cast<char*>(p), IoLen(n), 0, reinterpret_cast<sockaddr*>(from), &fl));
}

// Process id for seeds and temp names.
uint32_t process_id();

}  // namespace dcp::net::sock
