// util/net.cpp — cross-platform socket helpers (impl)
#include "net.hpp"
#include <string>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <cstring>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#endif

namespace nth::util::net {

namespace {

#ifdef _WIN32
// RAII for WSAStartup/WSACleanup. Constructed once globally; destructor
// runs at program exit.
struct WsaInit {
    bool ok;
    WsaInit() {
        WSADATA wsa;
        ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    }
    ~WsaInit() {
        if (ok) WSACleanup();
    }
};
WsaInit g_wsa;
#endif

} // namespace

bool init() {
#ifdef _WIN32
    return g_wsa.ok;
#else
    return true;
#endif
}

void close_socket(socket_t s) {
#ifdef _WIN32
    if (s != INVALID_SOCKET) closesocket(s);
#else
    if (s >= 0) ::close(s);
#endif
}

bool set_nonblocking(socket_t s) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

int last_sock_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

std::string sock_error(int err) {
#ifdef _WIN32
    LPWSTR buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER
                              | FORMAT_MESSAGE_FROM_SYSTEM
                              | FORMAT_MESSAGE_IGNORE_INSERTS,
                              nullptr, (DWORD)err, 0,
                              (LPWSTR)&buf, 0, nullptr);
    std::string out;
    if (n > 0 && buf) {
        // Wide → UTF-8
        int len = WideCharToMultiByte(CP_UTF8, 0, buf, (int)n,
                                      nullptr, 0, nullptr, nullptr);
        out.resize(len);
        WideCharToMultiByte(CP_UTF8, 0, buf, (int)n,
                            out.data(), len, nullptr, nullptr);
        LocalFree(buf);
    } else {
        out = "WSA error " + std::to_string(err);
    }
    return out;
#else
    return std::string(std::strerror(err));
#endif
}

} // namespace nth::util::net
