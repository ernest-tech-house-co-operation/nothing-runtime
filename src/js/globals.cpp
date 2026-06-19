// js/globals.cpp
#include "globals.hpp"
#include "engine.hpp"
#include "fetch.hpp"
#include "http_server.hpp"

#include <JavaScriptCore/JavaScript.h>

#include <iostream>
#include <cstring>
#include <unordered_map>
#include <mutex>

namespace nth::js::globals {

namespace {

// ---- console.log ----------------------------------------------------------

JSValueRef js_console_log(JSContextRef ctx, JSObjectRef function,
                          JSObjectRef thisObject, size_t argc,
                          const JSValueRef argv[], JSValueRef* exception) {
    (void)function; (void)thisObject; (void)exception;
    JSGlobalContextRef jctx = (JSGlobalContextRef)ctx;
    for (size_t i = 0; i < argc; ++i) {
        if (i) std::cout << ' ';
        // Use JSValue to JSON for objects so we see their structure.
        JSStringRef s = JSValueToStringCopy(jctx, argv[i], nullptr);
        size_t len = JSStringGetMaximumUTF8CStringSize(s);
        std::string buf(len, '\0');
        size_t actual = JSStringGetUTF8CString(s, buf.data(), len);
        JSStringRelease(s);
        if (actual > 0) buf.resize(actual - 1); else buf.clear();
        std::cout << buf;
    }
    std::cout << '\n';
    return JSValueMakeUndefined(ctx);
}

JSValueRef js_console_error(JSContextRef ctx, JSObjectRef function,
                            JSObjectRef thisObject, size_t argc,
                            const JSValueRef argv[], JSValueRef* exception) {
    (void)function; (void)thisObject; (void)exception;
    JSGlobalContextRef jctx = (JSGlobalContextRef)ctx;
    for (size_t i = 0; i < argc; ++i) {
        if (i) std::cerr << ' ';
        JSStringRef s = JSValueToStringCopy(jctx, argv[i], nullptr);
        size_t len = JSStringGetMaximumUTF8CStringSize(s);
        std::string buf(len, '\0');
        size_t actual = JSStringGetUTF8CString(s, buf.data(), len);
        JSStringRelease(s);
        if (actual > 0) buf.resize(actual - 1); else buf.clear();
        std::cerr << buf;
    }
    std::cerr << '\n';
    return JSValueMakeUndefined(ctx);
}

void install_console(JSGlobalContextRef ctx) {
    JSObjectRef global = JSContextGetGlobalObject(ctx);

    JSObjectRef console = JSObjectMake(ctx, nullptr, nullptr);

    auto add_fn = [&](const char* name, JSObjectCallAsFunctionCallback cb) {
        JSStringRef n = JSStringCreateWithUTF8CString(name);
        JSObjectRef fn = JSObjectMakeFunctionWithCallback(ctx, n, cb);
        JSObjectSetProperty(ctx, console, n, fn,
                            kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                            nullptr);
        JSStringRelease(n);
    };
    add_fn("log", js_console_log);
    add_fn("info", js_console_log);
    add_fn("warn", js_console_error);
    add_fn("error", js_console_error);
    add_fn("debug", js_console_log);

    JSStringRef console_name = JSStringCreateWithUTF8CString("console");
    JSObjectSetProperty(ctx, global, console_name, console,
                        kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                        nullptr);
    JSStringRelease(console_name);
}

// ---- Nth.serve + WebSocket helpers ---------------------------------------

// Nth.serve({ port, fetch(req)->Response, websocket? })
//
// We don't actually run the server here synchronously — `serve()` returns
// immediately and the server runs in the background. The C++ event loop is
// entered from main.cpp after the entry module finishes evaluating (the
// entry module's top-level code typically calls serve() to register a
// handler and then returns; we keep the process alive while the server
// runs).
//
// We stash the registered options object on a global registry keyed by ctx
// pointer.

void install_http_globals(JSGlobalContextRef ctx) {
    JSObjectRef global = JSContextGetGlobalObject(ctx);

    // Request / Response constructors
    JSObjectRef Request_ctor = fetch::make_request_constructor(ctx);
    JSObjectRef Response_ctor = fetch::make_response_constructor(ctx);
    {
        JSStringRef n = JSStringCreateWithUTF8CString("Request");
        JSObjectSetProperty(ctx, global, n, Request_ctor,
                            kJSPropertyAttributeDontDelete, nullptr);
        JSStringRelease(n);
    }
    {
        JSStringRef n = JSStringCreateWithUTF8CString("Response");
        JSObjectSetProperty(ctx, global, n, Response_ctor,
                            kJSPropertyAttributeDontDelete, nullptr);
        JSStringRelease(n);
    }

    // fetch(url, options)
    {
        JSStringRef n = JSStringCreateWithUTF8CString("fetch");
        JSObjectRef fn = JSObjectMakeFunctionWithCallback(ctx, n, fetch::js_fetch);
        JSObjectSetProperty(ctx, global, n, fn,
                            kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                            nullptr);
        JSStringRelease(n);
    }

    // Nth namespace
    JSObjectRef Nth = JSObjectMake(ctx, nullptr, nullptr);
    {
        JSStringRef n = JSStringCreateWithUTF8CString("serve");
        JSObjectRef fn = JSObjectMakeFunctionWithCallback(ctx, n, http_server::js_nth_serve);
        JSObjectSetProperty(ctx, Nth, n, fn,
                            kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                            nullptr);
        JSStringRelease(n);
    }
    {
        JSStringRef n = JSStringCreateWithUTF8CString("Nth");
        JSObjectSetProperty(ctx, global, n, Nth,
                            kJSPropertyAttributeDontDelete | kJSPropertyAttributeReadOnly,
                            nullptr);
        JSStringRelease(n);
    }
}

} // namespace

void install(JSGlobalContextRef ctx, bool http_enabled) {
    install_console(ctx);
    if (http_enabled) {
        install_http_globals(ctx);
    }
}

} // namespace nth::js::globals
