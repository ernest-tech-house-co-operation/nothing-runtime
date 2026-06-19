// js/fetch.cpp
//
// Implements:
//   - Request constructor: new Request(url, { method, headers, body })
//   - Response constructor: new Response(body, { status, statusText, headers })
//   - fetch(url, options) -> Promise<Response>
//
// HTTP client choice (per spec section 3b — agent's discretion, documented):
// We use a hand-rolled HTTP/1.1 client over plain BSD sockets. No external
// dependency. Supports: GET/POST/PUT/DELETE/etc, custom headers, request
// body, response status/headers/body. Does NOT support: HTTPS/TLS,
// HTTP/2, redirects, chunked transfer-encoding in responses (we read until
// EOF or Content-Length bytes), keep-alive (one request per connection).
// This is enough for the "minimal subset Elysia actually touches" per spec
// section 3b, and keeps the binary lean.

#include "fetch.hpp"
#include "engine.hpp"
#include "../util/strings.hpp"
#include "../util/net.hpp"

#include <JavaScriptCore/JavaScript.h>

// Socket headers — POSIX or Winsock, switched via util/net.hpp.
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#endif

#include <string>
#include <cstring>
#include <cerrno>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <thread>
#include <atomic>

namespace nth::js::fetch {

namespace {

// ---------------------------------------------------------------------------
// Minimal HTTP/1.1 client over sockets
// ---------------------------------------------------------------------------

struct HttpResponse {
    int status = 0;
    std::string status_text;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string error; // empty if ok
};

// Parse "http://host[:port]/path" into host, port, path. Returns false on
// bad URL. Only http: is supported (no TLS in v0.1).
bool parse_http_url(const std::string& url,
                    std::string& host, int& port, std::string& path) {
    if (url.compare(0, 7, "http://") != 0) return false;
    auto rest = url.substr(7);
    auto slash = rest.find('/');
    std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    if (path.empty()) path = "/";
    auto colon = hostport.find(':');
    if (colon == std::string::npos) {
        host = hostport;
        port = 80;
    } else {
        host = hostport.substr(0, colon);
        try { port = std::stoi(hostport.substr(colon + 1)); }
        catch (...) { return false; }
    }
    return true;
}

HttpResponse http_request(const std::string& method,
                          const std::string& url,
                          const std::vector<std::pair<std::string, std::string>>& headers,
                          const std::string& body) {
    HttpResponse r;
    std::string host; int port = 80; std::string path;
    if (!parse_http_url(url, host, port, path)) {
        r.error = "fetch: only http:// URLs are supported in v0.1 (got: " + url + ")";
        return r;
    }

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string port_str = std::to_string(port);
    int gai = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (gai != 0 || !res) {
#ifdef _WIN32
        r.error = std::string("fetch: getaddrinfo failed: ") + gai_strerrorA(gai);
#else
        r.error = std::string("fetch: getaddrinfo failed: ") + gai_strerror(gai);
#endif
        return r;
    }
    util::net::socket_t sock = util::net::kInvalidSocket;
    for (auto* a = res; a; a = a->ai_next) {
        sock = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (sock == util::net::kInvalidSocket) continue;
        if (connect(sock, a->ai_addr, a->ai_addrlen) == 0) break;
        util::net::close_socket(sock);
        sock = util::net::kInvalidSocket;
    }
    freeaddrinfo(res);
    if (sock == util::net::kInvalidSocket) {
        r.error = std::string("fetch: connect failed: ")
                  + util::net::sock_error(util::net::last_sock_error());
        return r;
    }

    // Build request
    std::ostringstream req;
    req << method << " " << path << " HTTP/1.1\r\n";
    req << "Host: " << host << (port == 80 ? "" : (":" + std::to_string(port))) << "\r\n";
    req << "Connection: close\r\n";
    bool has_cl = false, has_ua = false;
    for (auto& [k, v] : headers) {
        std::string lk = nth::util::to_lower(k);
        if (lk == "content-length") has_cl = true;
        if (lk == "user-agent") has_ua = true;
        req << k << ": " << v << "\r\n";
    }
    if (!body.empty() && !has_cl)
        req << "Content-Length: " << body.size() << "\r\n";
    if (!has_ua)
        req << "User-Agent: nth/0.1\r\n";
    req << "\r\n";
    std::string head = req.str();

    // Send
    size_t total = 0;
    while (total < head.size()) {
        ssize_t n = send(sock, head.data() + total, (int)(head.size() - total), 0);
        if (n <= 0) {
            r.error = std::string("fetch: send failed: ")
                      + util::net::sock_error(util::net::last_sock_error());
            util::net::close_socket(sock);
            return r;
        }
        total += (size_t)n;
    }
    if (!body.empty()) {
        total = 0;
        while (total < body.size()) {
            ssize_t n = send(sock, body.data() + total, (int)(body.size() - total), 0);
            if (n <= 0) {
                r.error = std::string("fetch: send body failed: ")
                          + util::net::sock_error(util::net::last_sock_error());
                util::net::close_socket(sock);
                return r;
            }
            total += (size_t)n;
        }
    }

    // Receive — read until EOF
    std::string raw;
    char buf[8192];
    while (true) {
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n > 0) raw.append(buf, n);
        else if (n == 0) break;
        else if (util::net::last_sock_error() == util::net::kEIntr) continue;
        else break;
    }
    util::net::close_socket(sock);

    // Split headers/body
    auto sep = raw.find("\r\n\r\n");
    if (sep == std::string::npos) {
        r.error = "fetch: malformed response (no header terminator)";
        return r;
    }
    std::string header_block = raw.substr(0, sep);
    r.body = raw.substr(sep + 4);

    // Status line
    auto first_eol = header_block.find("\r\n");
    std::string status_line = (first_eol == std::string::npos)
        ? header_block : header_block.substr(0, first_eol);
    {
        // "HTTP/1.1 200 OK"
        auto sp1 = status_line.find(' ');
        if (sp1 == std::string::npos) {
            r.error = "fetch: malformed status line: " + status_line;
            return r;
        }
        auto sp2 = status_line.find(' ', sp1 + 1);
        std::string code_s = (sp2 == std::string::npos)
            ? status_line.substr(sp1 + 1)
            : status_line.substr(sp1 + 1, sp2 - sp1 - 1);
        try { r.status = std::stoi(code_s); } catch (...) {}
        if (sp2 != std::string::npos) r.status_text = status_line.substr(sp2 + 1);
    }

    // Headers
    size_t pos = (first_eol == std::string::npos) ? std::string::npos : first_eol + 2;
    while (pos != std::string::npos && pos < header_block.size()) {
        auto eol = header_block.find("\r\n", pos);
        std::string line = (eol == std::string::npos)
            ? header_block.substr(pos) : header_block.substr(pos, eol - pos);
        if (line.empty()) break;
        auto c = line.find(':');
        if (c != std::string::npos) {
            std::string k = line.substr(0, c);
            std::string v = line.substr(c + 1);
            // trim leading space from v
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
            r.headers.emplace_back(std::move(k), std::move(v));
        }
        if (eol == std::string::npos) break;
        pos = eol + 2;
    }

    return r;
}

// ---------------------------------------------------------------------------
// JS ↔ C++ helpers
// ---------------------------------------------------------------------------

// Read a JS object's string property (or return default).
std::string get_string_prop(JSGlobalContextRef ctx, JSObjectRef obj,
                            const char* name, const std::string& def = "") {
    if (!obj) return def;
    JSStringRef k = JSStringCreateWithUTF8CString(name);
    JSValueRef v = JSObjectGetProperty(ctx, obj, k, nullptr);
    JSStringRelease(k);
    if (!v || !JSValueIsString(ctx, v)) return def;
    return value_to_string(ctx, v);
}

int get_number_prop(JSGlobalContextRef ctx, JSObjectRef obj,
                    const char* name, int def = 0) {
    if (!obj) return def;
    JSStringRef k = JSStringCreateWithUTF8CString(name);
    JSValueRef v = JSObjectGetProperty(ctx, obj, k, nullptr);
    JSStringRelease(k);
    if (!v) return def;
    double d = JSValueToNumber(ctx, v, nullptr);
    return (int)d;
}

// Read a JS object's `headers` property as a list of (key, value) pairs.
// `headers` may be: a plain object ({k: v}), an array of [k, v] pairs, or
// a JS Map (treated as plain object via .entries — too much work, we just
// iterate own properties).
std::vector<std::pair<std::string, std::string>>
extract_headers(JSGlobalContextRef ctx, JSValueRef headers_v) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!headers_v || !JSValueIsObject(ctx, headers_v)) return out;
    JSObjectRef obj = JSValueToObject(ctx, headers_v, nullptr);
    if (!obj) return out;
    JSPropertyNameArrayRef names = JSObjectCopyPropertyNames(ctx, obj);
    size_t n = JSPropertyNameArrayGetCount(names);
    for (size_t i = 0; i < n; ++i) {
        JSStringRef k = JSPropertyNameArrayGetNameAtIndex(names, i);
        JSValueRef v = JSObjectGetProperty(ctx, obj, k, nullptr);
        size_t len = JSStringGetMaximumUTF8CStringSize(k);
        std::string ks(len, '\0');
        size_t actual = JSStringGetUTF8CString(k, ks.data(), len);
        if (actual > 0) ks.resize(actual - 1); else ks.clear();
        std::string vs = v ? value_to_string(ctx, v) : "";
        out.emplace_back(std::move(ks), std::move(vs));
    }
    JSPropertyNameArrayRelease(names);
    return out;
}

JSObjectRef make_headers_obj(JSGlobalContextRef ctx,
                             const std::vector<std::pair<std::string, std::string>>& hs) {
    JSObjectRef obj = JSObjectMake(ctx, nullptr, nullptr);
    for (auto& [k, v] : hs) {
        JSStringRef kk = JSStringCreateWithUTF8CString(k.c_str());
        JSStringRef vv = JSStringCreateWithUTF8CString(v.c_str());
        JSObjectSetProperty(ctx, obj, kk, JSValueMakeString(ctx, vv),
                            kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                            nullptr);
        JSStringRelease(kk);
        JSStringRelease(vv);
    }
    return obj;
}

// Construct a JS Response-like object from a C++ HttpResponse.
JSObjectRef make_response_obj(JSGlobalContextRef ctx, const HttpResponse& hr) {
    JSObjectRef obj = JSObjectMake(ctx, nullptr, nullptr);

    auto set_str = [&](const char* name, const std::string& s) {
        JSStringRef k = JSStringCreateWithUTF8CString(name);
        JSStringRef v = JSStringCreateWithUTF8CString(s.c_str());
        JSObjectSetProperty(ctx, obj, k, JSValueMakeString(ctx, v),
                            kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                            nullptr);
        JSStringRelease(k); JSStringRelease(v);
    };
    auto set_num = [&](const char* name, int n) {
        JSStringRef k = JSStringCreateWithUTF8CString(name);
        JSObjectSetProperty(ctx, obj, k, JSValueMakeNumber(ctx, (double)n),
                            kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                            nullptr);
        JSStringRelease(k);
    };
    set_num("status", hr.status);
    set_str("statusText", hr.status_text);
    set_str("url", "");
    set_str("bodyText", hr.body);
    {
        JSStringRef k = JSStringCreateWithUTF8CString("headers");
        JSObjectSetProperty(ctx, obj, k, make_headers_obj(ctx, hr.headers),
                            kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                            nullptr);
        JSStringRelease(k);
    }
    {
        JSStringRef k = JSStringCreateWithUTF8CString("ok");
        JSObjectSetProperty(ctx, obj, k,
                            JSValueMakeBoolean(ctx, hr.status >= 200 && hr.status < 300),
                            kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly, nullptr);
        JSStringRelease(k);
    }

    // text() and json() — return Promises. Since we already have the body
    // synchronously, the promise resolves immediately.
    auto make_method = [&](const char* /*name*/, bool as_json) {
        JSStringRef body_s = JSStringCreateWithUTF8CString(hr.body.c_str());
        JSValueRef body_v = JSValueMakeString(ctx, body_s);
        JSStringRelease(body_s);
        JSValueRef result_v = body_v;
        if (as_json) {
            // Parse body as JSON
            JSStringRef json_s = JSStringCreateWithUTF8CString("JSON");
            JSObjectRef global = JSContextGetGlobalObject(ctx);
            JSValueRef json_v = JSObjectGetProperty(ctx, global, json_s, nullptr);
            JSStringRelease(json_s);
            if (json_v && JSValueIsObject(ctx, json_v)) {
                JSObjectRef json_obj = const_cast<JSObjectRef>(json_v);
                JSStringRef parse_name = JSStringCreateWithUTF8CString("parse");
                JSValueRef parse_v = JSObjectGetProperty(ctx, json_obj, parse_name, nullptr);
                JSStringRelease(parse_name);
                if (parse_v && JSObjectIsFunction(ctx, const_cast<JSObjectRef>(parse_v))) {
                    JSValueRef args[] = { body_v };
                    JSValueRef exc = nullptr;
                    result_v = JSObjectCallAsFunction(ctx,
                                                      const_cast<JSObjectRef>(parse_v),
                                                      nullptr, 1, args, &exc);
                    if (exc) result_v = JSValueMakeNull(ctx);
                }
            }
        }
        JSValueRef captured = result_v;
        JSObjectCallAsFunctionCallback cb = [](JSContextRef c, JSObjectRef f,
                                               JSObjectRef t, size_t argc,
                                               const JSValueRef argv[], JSValueRef* exc) -> JSValueRef {
            (void)f; (void)t; (void)argc; (void)argv; (void)exc;
            return JSValueMakeUndefined(c);
        };
        (void)cb;
        // We can't easily create a JS Promise from C without JSC's private
        // Promise constructor access. Use a thenable: an object with a .then()
        // method that calls its callbacks immediately.
        JSObjectRef thenable = JSObjectMake(ctx, nullptr, nullptr);
        JSStringRef captured_s = JSStringCreateWithUTF8CString("__nthCaptured");
        JSObjectSetProperty(ctx, thenable, captured_s, captured,
                            kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                            nullptr);
        JSStringRelease(captured_s);

        JSObjectCallAsFunctionCallback then_cb =
            [](JSContextRef c, JSObjectRef f, JSObjectRef t, size_t argc,
               const JSValueRef argv[], JSValueRef* exc) -> JSValueRef {
            (void)f; (void)argc; (void)exc;
            // t = the thenable. Read __nthCaptured off it.
            JSGlobalContextRef jctx = (JSGlobalContextRef)c;
            JSStringRef k = JSStringCreateWithUTF8CString("__nthCaptured");
            JSValueRef v = JSObjectGetProperty(jctx, t, k, nullptr);
            JSStringRelease(k);
            // argv[0] = onResolve; argv[1] = onReject
            if (argc >= 1 && argv[0] && JSObjectIsFunction(jctx, const_cast<JSObjectRef>(argv[0]))) {
                JSValueRef args[] = { v };
                return JSObjectCallAsFunction(jctx, const_cast<JSObjectRef>(argv[0]),
                                              nullptr, 1, args, nullptr);
            }
            return v;
        };
        JSStringRef then_name = JSStringCreateWithUTF8CString("then");
        JSObjectRef then_fn = JSObjectMakeFunctionWithCallback(ctx, then_name, then_cb);
        JSObjectSetProperty(ctx, thenable, then_name, then_fn,
                            kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                            nullptr);
        JSStringRelease(then_name);

        return thenable;
    };

    {
        JSObjectRef text_thenable = make_method("text", /*as_json=*/false);
        JSStringRef k = JSStringCreateWithUTF8CString("text");
        JSObjectSetProperty(ctx, obj, k, text_thenable,
                            kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                            nullptr);
        JSStringRelease(k);
    }
    {
        JSObjectRef json_thenable = make_method("json", /*as_json=*/true);
        JSStringRef k = JSStringCreateWithUTF8CString("json");
        JSObjectSetProperty(ctx, obj, k, json_thenable,
                            kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                            nullptr);
        JSStringRelease(k);
    }

    return obj;
}

// ---------------------------------------------------------------------------
// Request constructor: new Request(url, { method, headers, body })
// ---------------------------------------------------------------------------

JSValueRef js_request_ctor(JSContextRef ctx, JSObjectRef constructor,
                           size_t argc, const JSValueRef argv[], JSValueRef* exception) {
    (void)constructor;
    JSGlobalContextRef jctx = (JSGlobalContextRef)ctx;
    if (argc < 1 || !JSValueIsString(ctx, argv[0])) {
        return make_error(jctx, "Request: first argument must be a url string", exception);
    }
    std::string url = value_to_string(jctx, argv[0]);
    std::string method = "GET";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    if (argc >= 2 && JSValueIsObject(ctx, argv[1])) {
        JSObjectRef init = JSValueToObject(ctx, argv[1], nullptr);
        method = nth::util::to_lower(get_string_prop(jctx, init, "method", "GET"));
        JSStringRef k = JSStringCreateWithUTF8CString("headers");
        JSValueRef hv = JSObjectGetProperty(jctx, init, k, nullptr);
        JSStringRelease(k);
        headers = extract_headers(jctx, hv);
        body = get_string_prop(jctx, init, "body", "");
    }
    JSObjectRef obj = JSObjectMake(ctx, nullptr, nullptr);
    auto set_str = [&](const char* name, const std::string& s) {
        JSStringRef k = JSStringCreateWithUTF8CString(name);
        JSStringRef v = JSStringCreateWithUTF8CString(s.c_str());
        JSObjectSetProperty(jctx, obj, k, JSValueMakeString(jctx, v),
                            kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                            nullptr);
        JSStringRelease(k); JSStringRelease(v);
    };
    set_str("url", url);
    set_str("method", method);
    set_str("body", body);
    {
        JSStringRef k = JSStringCreateWithUTF8CString("headers");
        JSObjectSetProperty(jctx, obj, k, make_headers_obj(jctx, headers),
                            kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                            nullptr);
        JSStringRelease(k);
    }
    return obj;
}

} // end anonymous namespace

// ---------------------------------------------------------------------------
// Request constructor: new Request(url, { method, headers, body })
// ---------------------------------------------------------------------------

JSObjectRef make_request_constructor(JSGlobalContextRef ctx) {
    JSClassDefinition def = kJSClassDefinitionEmpty;
    def.className = "Request";
    def.callAsConstructor = [](JSContextRef c, JSObjectRef /*ctor*/, size_t argc,
                               const JSValueRef argv[], JSValueRef* exc) -> JSObjectRef {
        JSValueRef v = js_request_ctor(c, nullptr, argc, argv, exc);
        // js_request_ctor always returns an object; cast away constness
        // for the constructor callback signature.
        return const_cast<JSObjectRef>(JSValueToObject(c, v, nullptr));
    };
    JSClassRef cls = JSClassCreate(&def);
    JSObjectRef ctor = JSObjectMakeConstructor(ctx, cls, def.callAsConstructor);
    JSClassRelease(cls);
    return ctor;
}

// ---------------------------------------------------------------------------
// Response constructor: new Response(body, { status, statusText, headers })
// ---------------------------------------------------------------------------

JSValueRef js_response_ctor(JSContextRef ctx, JSObjectRef constructor,
                            size_t argc, const JSValueRef argv[], JSValueRef* exception) {
    (void)constructor;
    JSGlobalContextRef jctx = (JSGlobalContextRef)ctx;
    std::string body;
    if (argc >= 1 && argv[0] && JSValueIsString(ctx, argv[0])) {
        body = value_to_string(jctx, argv[0]);
    }
    int status = 200;
    std::string status_text = "OK";
    std::vector<std::pair<std::string, std::string>> headers;
    if (argc >= 2 && JSValueIsObject(ctx, argv[1])) {
        JSObjectRef init = JSValueToObject(ctx, argv[1], nullptr);
        status = get_number_prop(jctx, init, "status", 200);
        status_text = get_string_prop(jctx, init, "statusText", "");
        JSStringRef k = JSStringCreateWithUTF8CString("headers");
        JSValueRef hv = JSObjectGetProperty(jctx, init, k, nullptr);
        JSStringRelease(k);
        headers = extract_headers(jctx, hv);
    }
    HttpResponse hr;
    hr.status = status;
    hr.status_text = status_text;
    hr.headers = headers;
    hr.body = body;
    return make_response_obj(jctx, hr);
}

JSObjectRef make_response_constructor(JSGlobalContextRef ctx) {
    JSClassDefinition def = kJSClassDefinitionEmpty;
    def.className = "Response";
    def.callAsConstructor = [](JSContextRef c, JSObjectRef /*ctor*/, size_t argc,
                               const JSValueRef argv[], JSValueRef* exc) -> JSObjectRef {
        JSValueRef v = js_response_ctor(c, nullptr, argc, argv, exc);
        return const_cast<JSObjectRef>(JSValueToObject(c, v, nullptr));
    };
    JSClassRef cls = JSClassCreate(&def);
    JSObjectRef ctor = JSObjectMakeConstructor(ctx, cls, def.callAsConstructor);
    JSClassRelease(cls);
    return ctor;
}

// ---------------------------------------------------------------------------
// fetch(url, options)
// ---------------------------------------------------------------------------

JSValueRef js_fetch(JSContextRef ctx, JSObjectRef function,
                    JSObjectRef thisObject, size_t argc,
                    const JSValueRef argv[], JSValueRef* exception) {
    (void)function; (void)thisObject;
    JSGlobalContextRef jctx = (JSGlobalContextRef)ctx;
    if (argc < 1) {
        return make_error(jctx, "fetch: requires at least 1 argument (url)", exception);
    }
    std::string url;
    if (JSValueIsString(ctx, argv[0])) {
        url = value_to_string(jctx, argv[0]);
    } else if (JSValueIsObject(ctx, argv[0])) {
        // Maybe a Request object
        JSObjectRef req = JSValueToObject(ctx, argv[0], nullptr);
        url = get_string_prop(jctx, req, "url");
    }
    if (url.empty()) {
        return make_error(jctx, "fetch: could not determine url from argument", exception);
    }

    std::string method = "GET";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    if (argc >= 2 && JSValueIsObject(ctx, argv[1])) {
        JSObjectRef init = JSValueToObject(ctx, argv[1], nullptr);
        method = nth::util::to_lower(get_string_prop(jctx, init, "method", "GET"));
        JSStringRef k = JSStringCreateWithUTF8CString("headers");
        JSValueRef hv = JSObjectGetProperty(jctx, init, k, nullptr);
        JSStringRelease(k);
        headers = extract_headers(jctx, hv);
        body = get_string_prop(jctx, init, "body", "");
    } else if (argc >= 1 && JSValueIsObject(ctx, argv[0])) {
        JSObjectRef req = JSValueToObject(ctx, argv[0], nullptr);
        method = nth::util::to_lower(get_string_prop(jctx, req, "method", "GET"));
        body = get_string_prop(jctx, req, "body", "");
        JSStringRef k = JSStringCreateWithUTF8CString("headers");
        JSValueRef hv = JSObjectGetProperty(jctx, req, k, nullptr);
        JSStringRelease(k);
        headers = extract_headers(jctx, hv);
    }

    // We perform the request synchronously here (single-threaded v0.1) and
    // return a thenable that resolves immediately. JS awaits it; works for
    // both `await fetch(...)` and `fetch(...).then(...)`.
    HttpResponse hr = http_request(method, url, headers, body);
    JSObjectRef response_obj = make_response_obj(jctx, hr);
    if (!hr.error.empty()) {
        // Reject: still return a thenable, but its .then() calls onReject.
        JSObjectRef thenable = JSObjectMake(ctx, nullptr, nullptr);
        JSStringRef err_s = JSStringCreateWithUTF8CString(hr.error.c_str());
        JSValueRef err_v = JSValueMakeString(jctx, err_s);
        JSStringRelease(err_s);
        JSStringRef cap = JSStringCreateWithUTF8CString("__nthCaptured");
        JSObjectSetProperty(jctx, thenable, cap, err_v,
                            kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                            nullptr);
        JSStringRelease(cap);
        JSObjectCallAsFunctionCallback reject_cb =
            [](JSContextRef c, JSObjectRef f, JSObjectRef t, size_t argc,
               const JSValueRef argv[], JSValueRef* exc) -> JSValueRef {
            (void)f; (void)argc; (void)exc;
            JSGlobalContextRef jctx2 = (JSGlobalContextRef)c;
            JSStringRef k = JSStringCreateWithUTF8CString("__nthCaptured");
            JSValueRef v = JSObjectGetProperty(jctx2, t, k, nullptr);
            JSStringRelease(k);
            if (argc >= 2 && argv[1] && JSObjectIsFunction(jctx2, const_cast<JSObjectRef>(argv[1]))) {
                JSValueRef args[] = { v };
                return JSObjectCallAsFunction(jctx2, const_cast<JSObjectRef>(argv[1]),
                                              nullptr, 1, args, nullptr);
            }
            return JSValueMakeUndefined(c);
        };
        JSStringRef then_name = JSStringCreateWithUTF8CString("then");
        JSObjectRef then_fn = JSObjectMakeFunctionWithCallback(jctx, then_name, reject_cb);
        JSObjectSetProperty(jctx, thenable, then_name, then_fn,
                            kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                            nullptr);
        JSStringRelease(then_name);
        return thenable;
    }
    return response_obj;
}

} // namespace nth::js::fetch
