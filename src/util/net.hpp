// util/net.hpp — cross-platform socket helpers
//
// BSD sockets (Linux/macOS) and Winsock (Windows) use the same names
// for most calls (socket/bind/listen/accept/send/recv) but differ in:
//   - Initialization (WSAStartup on Windows, none on POSIX)
//   - Close (closesocket on Windows, close on POSIX)
//   - Non-blocking mode (ioctlsocket on Windows, fcntl on POSIX)
//   - errno semantics (WSAGetLastError on Windows for socket calls)
//
// This header provides thin wrappers so the rest of the codebase can
// stay #ifdef-free. Calls that already work identically (send/recv/
// getaddrinfo/htons/htonl) are NOT re-wrapped — they're used directly
// from the standard headers.
#pragma once

#include <string>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#endif

namespace nth::util::net {

#ifdef _WIN32
   using socket_t = SOCKET;
   constexpr socket_t kInvalidSocket = INVALID_SOCKET;
   constexpr int kEWouldBlock = WSAEWOULDBLOCK;
   constexpr int kEIntr       = WSAEINTR;
#else
   using socket_t = int;
   constexpr socket_t kInvalidSocket = -1;
   constexpr int kEWouldBlock = EWOULDBLOCK;
   constexpr int kEIntr       = EINTR;
#endif

// One-time Winsock initialization. On POSIX it's a no-op. Call once at
// program startup; safe to call multiple times (subsequent calls just
// bump an internal refcount).
bool init();

// Close a socket. Use this instead of close()/closesocket() directly.
void close_socket(socket_t s);

// Set a socket to non-blocking mode.
bool set_nonblocking(socket_t s);

// Convert a socket-related error code into a human-readable string.
// On Windows, pass WSAGetLastError(); on POSIX, pass errno.
std::string sock_error(int err);

// Return the current socket error (WSAGetLastError on Windows, errno
// on POSIX).
int last_sock_error();

} // namespace nth::util::net
