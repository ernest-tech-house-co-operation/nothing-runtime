// server/websocket.cpp
//
// Minimal RFC 6455 server-side implementation: handshake (SHA-1 + base64
// of the magic GUID), frame parsing (text/binary/close/ping/pong), and
// a dispatch loop that hands messages to the JS handler object.
//
// SHA-1 + base64 are vendored in src/util/sha1.cpp (no OpenSSL dep) —
// this keeps the build simpler on both Linux and Windows.
//
// Frame size limit for v0.1: 1MB per message. Larger messages will be
// rejected with a close frame. No fragmentation reassembly across frames.
//
// TLS / WSS is NOT supported in v0.1 (consistent with spec section 6).

#include "websocket.hpp"
#include "http.hpp"
#include "../js/engine.hpp"
#include "../util/strings.hpp"
#include "../util/sha1.hpp"
#include "../util/net.hpp"

#include <JavaScriptCore/JavaScript.h>

// Socket headers — POSIX or Winsock, switched via util/net.hpp.
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#else
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include <cstdint>
#include <cstring>
#include <cerrno>
#include <string>
#include <vector>
#include <sstream>
#include <iostream>

namespace nth::server {

namespace {

std::string compute_accept_impl(const std::string& key) {
    static const std::string MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string concat = key + MAGIC;
    std::string digest = nth::util::sha1(concat);
    return nth::util::base64_encode(
        reinterpret_cast<const uint8_t*>(digest.data()), digest.size());
}

// Build a JS "peer" object with .send(text) and .close() methods.
// Internally we capture the fd and ctx via private properties.
JSObjectRef make_peer_obj(JSGlobalContextRef ctx, util::net::socket_t fd) {
    JSObjectRef peer = JSObjectMake(ctx, nullptr, nullptr);
    // Capture fd as a private number property.
    JSStringRef fd_key = JSStringCreateWithUTF8CString("__nthFd");
    JSObjectSetProperty(ctx, peer, fd_key, JSValueMakeNumber(ctx, (double)(intptr_t)fd),
                        kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontEnum,
                        nullptr);
    JSStringRelease(fd_key);

    JSObjectCallAsFunctionCallback send_cb =
        [](JSContextRef c, JSObjectRef f, JSObjectRef t, size_t argc,
           const JSValueRef argv[], JSValueRef* exc) -> JSValueRef {
        (void)f; (void)argc; (void)exc;
        JSGlobalContextRef jctx = (JSGlobalContextRef)c;
        if (argc < 1 || !argv[0]) return JSValueMakeUndefined(c);
        std::string data;
        if (JSValueIsString(c, argv[0])) {
            data = nth::js::value_to_string(jctx, argv[0]);
        } else {
            JSStringRef s = JSValueToStringCopy(jctx, argv[0], nullptr);
            size_t len = JSStringGetMaximumUTF8CStringSize(s);
            data.resize(len);
            size_t actual = JSStringGetUTF8CString(s, data.data(), len);
            JSStringRelease(s);
            if (actual > 0) data.resize(actual - 1); else data.clear();
        }
        JSStringRef k = JSStringCreateWithUTF8CString("__nthFd");
        JSValueRef fdv = JSObjectGetProperty(jctx, t, k, nullptr);
        JSStringRelease(k);
        if (!fdv) return JSValueMakeUndefined(c);
        util::net::socket_t fd = (util::net::socket_t)(intptr_t)JSValueToNumber(jctx, fdv, nullptr);

        // Build a text frame (opcode 0x1, FIN=1, mask=0 from server).
        std::vector<uint8_t> frame;
        frame.push_back(0x81);
        size_t len = data.size();
        if (len < 126) {
            frame.push_back((uint8_t)len);
        } else if (len <= 65535) {
            frame.push_back(126);
            frame.push_back((uint8_t)((len >> 8) & 0xFF));
            frame.push_back((uint8_t)(len & 0xFF));
        } else {
            frame.push_back(127);
            for (int i = 7; i >= 0; --i)
                frame.push_back((uint8_t)((len >> (8 * i)) & 0xFF));
        }
        frame.insert(frame.end(), data.begin(), data.end());
        size_t total = 0;
        while (total < frame.size()) {
            ssize_t n = send(fd, frame.data() + total,
                             (int)(frame.size() - total), 0);
            if (n <= 0) break;
            total += (size_t)n;
        }
        return JSValueMakeUndefined(c);
    };
    JSStringRef send_name = JSStringCreateWithUTF8CString("send");
    JSObjectRef send_fn = JSObjectMakeFunctionWithCallback(ctx, send_name, send_cb);
    JSObjectSetProperty(ctx, peer, send_name, send_fn,
                        kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                        nullptr);
    JSStringRelease(send_name);

    JSObjectCallAsFunctionCallback close_cb =
        [](JSContextRef c, JSObjectRef f, JSObjectRef t, size_t argc,
           const JSValueRef argv[], JSValueRef* exc) -> JSValueRef {
        (void)f; (void)argc; (void)argv; (void)exc;
        JSGlobalContextRef jctx = (JSGlobalContextRef)c;
        JSStringRef k = JSStringCreateWithUTF8CString("__nthFd");
        JSValueRef fdv = JSObjectGetProperty(jctx, t, k, nullptr);
        JSStringRelease(k);
        if (fdv) {
            util::net::socket_t fd = (util::net::socket_t)(intptr_t)JSValueToNumber(jctx, fdv, nullptr);
            uint8_t close_frame[] = {0x88, 0x00};
            send(fd, (const char*)close_frame, sizeof(close_frame), 0);
            shutdown(fd,
#ifdef _WIN32
                SD_BOTH
#else
                SHUT_RDWR
#endif
            );
        }
        return JSValueMakeUndefined(c);
    };
    JSStringRef close_name = JSStringCreateWithUTF8CString("close");
    JSObjectRef close_fn = JSObjectMakeFunctionWithCallback(ctx, close_name, close_cb);
    JSObjectSetProperty(ctx, peer, close_name, close_fn,
                        kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                        nullptr);
    JSStringRelease(close_name);

    return peer;
}

// Send the 101 Switching Protocols response.
bool send_handshake_response(util::net::socket_t fd, const std::string& accept_value) {
    std::ostringstream out;
    out << "HTTP/1.1 101 Switching Protocols\r\n"
        << "Upgrade: websocket\r\n"
        << "Connection: Upgrade\r\n"
        << "Sec-WebSocket-Accept: " << accept_value << "\r\n"
        << "\r\n";
    std::string s = out.str();
    size_t total = 0;
    while (total < s.size()) {
        ssize_t n = send(fd, s.data() + total, (int)(s.size() - total), 0);
        if (n <= 0) return false;
        total += (size_t)n;
    }
    return true;
}

// Read exactly `n` bytes from `fd`. Returns false on EOF/error.
bool read_n(util::net::socket_t fd, uint8_t* buf, size_t n) {
    size_t total = 0;
    while (total < n) {
        ssize_t r = recv(fd, (char*)buf + total, (int)(n - total), 0);
        if (r <= 0) return false;
        total += (size_t)r;
    }
    return true;
}

void call_handler(JSGlobalContextRef ctx, JSObjectRef handler, const char* name,
                  int argc, JSValueRef argv[]) {
    if (!handler) return;
    JSStringRef k = JSStringCreateWithUTF8CString(name);
    JSValueRef v = JSObjectGetProperty(ctx, handler, k, nullptr);
    JSStringRelease(k);
    if (!v || !JSValueIsObject(ctx, v)) return;
    JSObjectRef fn = JSValueToObject(ctx, v, nullptr);
    if (!fn || !JSObjectIsFunction(ctx, fn)) return;
    JSValueRef exc = nullptr;
    JSObjectCallAsFunction(ctx, fn, nullptr, argc, argv, &exc);
    if (exc) {
        std::cerr << "[nth.ws] handler " << name << " threw: "
                  << nth::js::value_to_string(ctx, exc) << "\n";
    }
}

} // namespace

std::string compute_accept(const std::string& key) {
    return compute_accept_impl(key);
}

void websocket_handshake_and_loop(JSGlobalContextRef ctx, util::net::socket_t fd,
                                  const std::string& sec_websocket_key,
                                  JSObjectRef handler) {
    std::string accept = compute_accept_impl(sec_websocket_key);
    if (!send_handshake_response(fd, accept)) return;

    JSObjectRef peer = make_peer_obj(ctx, fd);
    {
        JSValueRef a[] = { peer };
        call_handler(ctx, handler, "open", 1, a);
    }

    while (true) {
        uint8_t header[2];
        if (!read_n(fd, header, 2)) break;
        bool fin = (header[0] & 0x80) != 0;
        uint8_t opcode = header[0] & 0x0F;
        bool masked = (header[1] & 0x80) != 0;
        uint64_t len = header[1] & 0x7F;
        if (len == 126) {
            uint8_t ext[2];
            if (!read_n(fd, ext, 2)) break;
            len = ((uint64_t)ext[0] << 8) | ext[1];
        } else if (len == 127) {
            uint8_t ext[8];
            if (!read_n(fd, ext, 8)) break;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
        }
        if (len > 1024 * 1024) break; // 1MB limit
        uint8_t mask[4] = {0,0,0,0};
        if (masked) {
            if (!read_n(fd, mask, 4)) break;
        }
        std::vector<uint8_t> payload((size_t)len);
        if (len > 0 && !read_n(fd, payload.data(), (size_t)len)) break;
        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i)
                payload[i] ^= mask[i % 4];
        }

        (void)fin;
        if (opcode == 0x8) {
            JSValueRef a[] = { peer };
            call_handler(ctx, handler, "close", 1, a);
            break;
        } else if (opcode == 0x9) {
            // Ping → respond with pong of same payload
            uint8_t pong[2] = {0x8A, (uint8_t)payload.size()};
            send(fd, (const char*)pong, 2, 0);
            if (!payload.empty())
                send(fd, (const char*)payload.data(), (int)payload.size(), 0);
            continue;
        } else if (opcode == 0xA) {
            continue;
        } else if (opcode == 0x1 || opcode == 0x2) {
            std::string s(payload.begin(), payload.end());
            JSStringRef sref = JSStringCreateWithUTF8CString(s.c_str());
            JSValueRef msg = JSValueMakeString(ctx, sref);
            JSStringRelease(sref);
            JSValueRef a[] = { peer, msg };
            call_handler(ctx, handler, "message", 2, a);
        }
    }
    shutdown(fd,
#ifdef _WIN32
        SD_BOTH
#else
        SHUT_RDWR
#endif
    );
}

} // namespace nth::server
