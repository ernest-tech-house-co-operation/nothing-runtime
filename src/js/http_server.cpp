// js/http_server.cpp
//
// Native HTTP server surface for `Nth.serve()`. Single-threaded event
// loop using select(2) — portable across POSIX (Linux/macOS) and Winsock.
// WebSocket upgrade is supported via the `websocket` handler object
// passed to serve() (open/message/close callbacks).
//
// The C++ side is deliberately minimal per spec section 3b: accept sockets,
// parse HTTP requests minimally, hand a Request object to the JS `fetch`
// callback, send back whatever Response comes back. No routing, no
// middleware — that's Elysia's job in JS.

#include "http_server.hpp"
#include "engine.hpp"
#include "fetch.hpp"
#include "../server/http.hpp"
#include "../server/websocket.hpp"
#include "../util/net.hpp"
#include "../util/strings.hpp"

#include <JavaScriptCore/JavaScript.h>

// Socket headers — POSIX or Winsock, switched via util/net.hpp.
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#endif
#include <signal.h>

#include <iostream>
#include <cstring>
#include <cerrno>
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>

namespace nth::js::http_server {

namespace {

// Per-ctx server registry. v0.1 only supports one server per process; we
// still store as a list for forward-compat.
struct ServerRecord {
    int port = 0;
    JSObjectRef fetch_handler = nullptr;       // stable JS callback
    JSObjectRef websocket_handler = nullptr;   // optional
    JSGlobalContextRef ctx = nullptr;
    util::net::socket_t listen_fd = util::net::kInvalidSocket;
};
std::vector<std::unique_ptr<ServerRecord>>& servers() {
    static std::vector<std::unique_ptr<ServerRecord>> s;
    return s;
}

std::atomic<bool> g_stop_requested{false};

void on_sigint(int) {
    g_stop_requested = true;
}

// Read a JS object's number property.
int get_num_prop(JSGlobalContextRef ctx, JSObjectRef obj, const char* name, int def) {
    if (!obj) return def;
    JSStringRef k = JSStringCreateWithUTF8CString(name);
    JSValueRef v = JSObjectGetProperty(ctx, obj, k, nullptr);
    JSStringRelease(k);
    if (!v) return def;
    return (int)JSValueToNumber(ctx, v, nullptr);
}

JSObjectRef get_obj_prop(JSGlobalContextRef ctx, JSObjectRef obj, const char* name) {
    if (!obj) return nullptr;
    JSStringRef k = JSStringCreateWithUTF8CString(name);
    JSValueRef v = JSObjectGetProperty(ctx, obj, k, nullptr);
    JSStringRelease(k);
    if (!v || !JSValueIsObject(ctx, v)) return nullptr;
    return JSValueToObject(ctx, v, nullptr);
}

JSObjectRef get_func_prop(JSGlobalContextRef ctx, JSObjectRef obj, const char* name) {
    JSObjectRef o = get_obj_prop(ctx, obj, name);
    if (!o) return nullptr;
    if (!JSObjectIsFunction(ctx, o)) return nullptr;
    return o;
}

// Build a JS Request object from a parsed HttpRequest. Re-uses the Request
// constructor's logic by just constructing a plain object with the same
// shape (since we don't actually need `instanceof Request`).
JSObjectRef build_request_obj(JSGlobalContextRef ctx, const server::HttpRequest& req) {
    JSObjectRef obj = JSObjectMake(ctx, nullptr, nullptr);
    auto set_str = [&](const char* name, const std::string& s) {
        JSStringRef k = JSStringCreateWithUTF8CString(name);
        JSStringRef v = JSStringCreateWithUTF8CString(s.c_str());
        JSObjectSetProperty(ctx, obj, k, JSValueMakeString(ctx, v),
                            kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                            nullptr);
        JSStringRelease(k); JSStringRelease(v);
    };
    set_str("url", req.path);
    set_str("method", req.method);
    set_str("body", req.body);
    // headers as object
    JSObjectRef headers_obj = JSObjectMake(ctx, nullptr, nullptr);
    for (auto& [k, v] : req.headers) {
        JSStringRef kk = JSStringCreateWithUTF8CString(k.c_str());
        JSStringRef vv = JSStringCreateWithUTF8CString(v.c_str());
        JSObjectSetProperty(ctx, headers_obj, kk, JSValueMakeString(ctx, vv),
                            kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                            nullptr);
        JSStringRelease(kk); JSStringRelease(vv);
    }
    JSStringRef hk = JSStringCreateWithUTF8CString("headers");
    JSObjectSetProperty(ctx, obj, hk, headers_obj,
                        kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                        nullptr);
    JSStringRelease(hk);
    return obj;
}

// Read the JS Response object returned by the fetch handler and serialize
// it into an HttpResponse for writing back to the socket.
server::HttpResponse response_obj_to_http(JSGlobalContextRef ctx, JSObjectRef obj) {
    server::HttpResponse r;
    if (!obj) {
        r.status = 500;
        r.body = "Internal: handler returned no response";
        return r;
    }
    JSStringRef k_status = JSStringCreateWithUTF8CString("status");
    JSStringRef k_body = JSStringCreateWithUTF8CString("bodyText");
    JSStringRef k_headers = JSStringCreateWithUTF8CString("headers");
    JSValueRef v_status = JSObjectGetProperty(ctx, obj, k_status, nullptr);
    JSValueRef v_body = JSObjectGetProperty(ctx, obj, k_body, nullptr);
    JSValueRef v_headers = JSObjectGetProperty(ctx, obj, k_headers, nullptr);
    JSStringRelease(k_status);
    JSStringRelease(k_body);
    JSStringRelease(k_headers);
    if (v_status) r.status = (int)JSValueToNumber(ctx, v_status, nullptr);
    if (r.status == 0) r.status = 200;
    if (v_body && JSValueIsString(ctx, v_body)) r.body = value_to_string(ctx, v_body);
    // If bodyText wasn't set, try "body" too
    if (r.body.empty()) {
        JSStringRef k_body2 = JSStringCreateWithUTF8CString("body");
        JSValueRef v_body2 = JSObjectGetProperty(ctx, obj, k_body2, nullptr);
        JSStringRelease(k_body2);
        if (v_body2 && JSValueIsString(ctx, v_body2))
            r.body = value_to_string(ctx, v_body2);
    }
    if (v_headers && JSValueIsObject(ctx, v_headers)) {
        JSObjectRef hobj = JSValueToObject(ctx, v_headers, nullptr);
        JSPropertyNameArrayRef names = JSObjectCopyPropertyNames(ctx, hobj);
        size_t n = JSPropertyNameArrayGetCount(names);
        for (size_t i = 0; i < n; ++i) {
            JSStringRef k = JSPropertyNameArrayGetNameAtIndex(names, i);
            JSValueRef v = JSObjectGetProperty(ctx, hobj, k, nullptr);
            size_t len = JSStringGetMaximumUTF8CStringSize(k);
            std::string ks(len, '\0');
            size_t actual = JSStringGetUTF8CString(k, ks.data(), len);
            if (actual > 0) ks.resize(actual - 1); else ks.clear();
            std::string vs = v ? value_to_string(ctx, v) : "";
            r.headers.emplace_back(std::move(ks), std::move(vs));
        }
        JSPropertyNameArrayRelease(names);
    }
    return r;
}

// Send a complete HttpResponse over the socket.
bool send_response(util::net::socket_t fd, const server::HttpResponse& r) {
    std::ostringstream out;
    out << "HTTP/1.1 " << r.status << " "
        << (r.status_text.empty() ? server::status_text(r.status) : r.status_text)
        << "\r\n";
    bool has_cl = false, has_ct = false;
    for (auto& [k, v] : r.headers) {
        if (nth::util::to_lower(k) == "content-length") has_cl = true;
        if (nth::util::to_lower(k) == "content-type") has_ct = true;
        out << k << ": " << v << "\r\n";
    }
    if (!has_cl) out << "Content-Length: " << r.body.size() << "\r\n";
    if (!has_ct && !r.body.empty()) out << "Content-Type: text/plain; charset=utf-8\r\n";
    out << "Connection: close\r\n\r\n";
    out << r.body;
    std::string s = out.str();
    size_t total = 0;
    while (total < s.size()) {
        ssize_t n = send(fd, s.data() + total, (int)(s.size() - total), 0);
        if (n <= 0) return false;
        total += (size_t)n;
    }
    return true;
}

void handle_connection(ServerRecord& srv, util::net::socket_t client_fd) {
    // Read until we have headers, then read body per Content-Length.
    server::HttpRequest req;
    std::string raw;
    char buf[4096];
    size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) { util::net::close_socket(client_fd); return; }
        raw.append(buf, n);
        header_end = raw.find("\r\n\r\n");
        if (header_end != std::string::npos) break;
        if (raw.size() > 64 * 1024) {
            server::HttpResponse r; r.status = 431; r.body = "Headers too large";
            send_response(client_fd, r);
            util::net::close_socket(client_fd);
            return;
        }
    }
    if (!server::parse_request(raw.substr(0, header_end), req)) {
        server::HttpResponse r; r.status = 400; r.body = "Bad request";
        send_response(client_fd, r);
        util::net::close_socket(client_fd);
        return;
    }
    // Read body if Content-Length is set.
    std::string rest = raw.substr(header_end + 4);
    size_t content_length = 0;
    for (auto& [k, v] : req.headers) {
        if (nth::util::to_lower(k) == "content-length") {
            try { content_length = std::stoul(v); } catch (...) {}
        }
    }
    while (rest.size() < content_length) {
        ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        rest.append(buf, n);
    }
    req.body = rest.substr(0, content_length);

    // WebSocket upgrade?
    bool is_ws_upgrade = false;
    std::string ws_key;
    for (auto& [k, v] : req.headers) {
        if (nth::util::to_lower(k) == "upgrade" && nth::util::to_lower(v) == "websocket") {
            is_ws_upgrade = true;
        }
        if (nth::util::to_lower(k) == "sec-websocket-key") ws_key = v;
    }
    if (is_ws_upgrade && srv.websocket_handler) {
        server::websocket_handshake_and_loop(srv.ctx, client_fd, ws_key,
                                              srv.websocket_handler);
        util::net::close_socket(client_fd);
        return;
    }

    // Normal HTTP: call the JS fetch handler.
    if (!srv.fetch_handler) {
        server::HttpResponse r; r.status = 500; r.body = "No fetch handler registered";
        send_response(client_fd, r);
        util::net::close_socket(client_fd);
        return;
    }
    JSObjectRef req_obj = build_request_obj(srv.ctx, req);
    JSValueRef args[] = { req_obj };
    JSValueRef exc = nullptr;
    JSValueRef ret = JSObjectCallAsFunction(srv.ctx, srv.fetch_handler,
                                            nullptr, 1, args, &exc);
    if (exc) {
        std::string msg = value_to_string(srv.ctx, exc);
        server::HttpResponse r; r.status = 500;
        r.body = "Internal: handler threw: " + msg;
        send_response(client_fd, r);
    } else {
        JSObjectRef resp_obj = (ret && JSValueIsObject(srv.ctx, ret))
            ? JSValueToObject(srv.ctx, ret, nullptr) : nullptr;
        server::HttpResponse hr = response_obj_to_http(srv.ctx, resp_obj);
        send_response(client_fd, hr);
    }
    util::net::close_socket(client_fd);
}

} // namespace

JSValueRef js_nth_serve(JSContextRef ctx, JSObjectRef function,
                        JSObjectRef thisObject, size_t argc,
                        const JSValueRef argv[], JSValueRef* exception) {
    (void)function; (void)thisObject;
    JSGlobalContextRef jctx = (JSGlobalContextRef)ctx;
    if (argc < 1 || !JSValueIsObject(ctx, argv[0])) {
        return make_error(jctx, "Nth.serve: requires an options object", exception);
    }
    JSObjectRef opts = JSValueToObject(ctx, argv[0], nullptr);
    int port = get_num_prop(jctx, opts, "port", 3000);
    JSObjectRef fetch_fn = get_func_prop(jctx, opts, "fetch");
    JSObjectRef ws_handler = get_obj_prop(jctx, opts, "websocket");
    if (!fetch_fn) {
        return make_error(jctx, "Nth.serve: options.fetch must be a function", exception);
    }

    // Open listening socket now so we can fail fast if the port is taken.
    util::net::socket_t listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == util::net::kInvalidSocket) {
        return make_error(jctx, std::string("socket(): ")
                          + util::net::sock_error(util::net::last_sock_error()),
                          exception);
    }
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&yes, sizeof(yes));
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::string e = std::string("bind(") + std::to_string(port) + "): "
                        + util::net::sock_error(util::net::last_sock_error());
        util::net::close_socket(listen_fd);
        return make_error(jctx, e, exception);
    }
    if (listen(listen_fd, 64) < 0) {
        std::string e = std::string("listen(): ")
                        + util::net::sock_error(util::net::last_sock_error());
        util::net::close_socket(listen_fd);
        return make_error(jctx, e, exception);
    }
    // Non-blocking accept so we can poll for Ctrl-C.
    util::net::set_nonblocking(listen_fd);

    auto rec = std::make_unique<ServerRecord>();
    rec->port = port;
    rec->fetch_handler = const_cast<JSObjectRef>(fetch_fn);
    JSValueProtect(jctx, rec->fetch_handler);
    if (ws_handler) {
        rec->websocket_handler = ws_handler;
        JSValueProtect(jctx, rec->websocket_handler);
    }
    rec->ctx = jctx;
    rec->listen_fd = listen_fd;
    servers().push_back(std::move(rec));

    std::cerr << "[nth] server listening on http://0.0.0.0:" << port << "\n";

    // Return a handle object with .stop() and .port.
    JSObjectRef handle = JSObjectMake(ctx, nullptr, nullptr);
    {
        JSStringRef k = JSStringCreateWithUTF8CString("port");
        JSObjectSetProperty(jctx, handle, k, JSValueMakeNumber(jctx, (double)port),
                            kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                            nullptr);
        JSStringRelease(k);
    }
    // .stop() — sets the global stop flag.
    JSObjectCallAsFunctionCallback stop_cb =
        [](JSContextRef c, JSObjectRef f, JSObjectRef t, size_t argc,
           const JSValueRef argv[], JSValueRef* exc) -> JSValueRef {
        (void)c; (void)f; (void)t; (void)argc; (void)argv; (void)exc;
        g_stop_requested = true;
        return JSValueMakeUndefined(c);
    };
    JSStringRef stop_name = JSStringCreateWithUTF8CString("stop");
    JSObjectRef stop_fn = JSObjectMakeFunctionWithCallback(jctx, stop_name, stop_cb);
    JSObjectSetProperty(jctx, handle, stop_name, stop_fn,
                        kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                        nullptr);
    JSStringRelease(stop_name);

    return handle;
}

int run_registered_servers(JSGlobalContextRef ctx) {
    (void)ctx;
    if (servers().empty()) return 0;

    // Install SIGINT handler so Ctrl-C shuts us down cleanly.
    // On Windows we use signal() since sigaction isn't available.
#ifndef _WIN32
    struct sigaction sa{};
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#else
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
#endif

    while (!g_stop_requested) {
        // Use select() with a 250ms timeout — portable across POSIX and
        // Winsock. WSAPoll exists but is notoriously buggy.
        fd_set rfds;
        FD_ZERO(&rfds);
        // Track max fd for select() — only needed on POSIX; on Winsock
        // the fd_set is a fixed-size array indexed by socket value, so
        // we just check the count.
        util::net::socket_t max_fd = util::net::kInvalidSocket;
        for (auto& s : servers()) {
            FD_SET(s->listen_fd, &rfds);
            if (s->listen_fd > max_fd || max_fd == util::net::kInvalidSocket) {
                max_fd = s->listen_fd;
            }
        }
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 250 * 1000; // 250ms

#ifdef _WIN32
        int n = select(0, &rfds, nullptr, nullptr, &tv);
#else
        int n = select((int)max_fd + 1, &rfds, nullptr, nullptr, &tv);
#endif
        if (n < 0) {
            int e = util::net::last_sock_error();
            if (e == util::net::kEIntr) continue;
            break;
        }
        if (n == 0) continue;
        for (size_t i = 0; i < servers().size(); ++i) {
            util::net::socket_t lfd = servers()[i]->listen_fd;
            if (lfd == util::net::kInvalidSocket) continue;
            if (FD_ISSET(lfd, &rfds)) {
                sockaddr_in caddr{};
                socklen_t clen = sizeof(caddr);
                util::net::socket_t cfd = accept(lfd, (sockaddr*)&caddr, &clen);
                if (cfd == util::net::kInvalidSocket) {
                    int e = util::net::last_sock_error();
                    if (e == util::net::kEWouldBlock
#ifdef _WIN32
                        || e == WSAECONNRESET
#endif
                    ) continue;
                    continue;
                }
                handle_connection(*servers()[i], cfd);
            }
        }
    }

    // Cleanup
    for (auto& s : servers()) {
        if (s->listen_fd != util::net::kInvalidSocket) {
            util::net::close_socket(s->listen_fd);
            s->listen_fd = util::net::kInvalidSocket;
        }
    }
    std::cerr << "[nth] server stopped\n";
    return 0;
}

} // namespace nth::js::http_server
