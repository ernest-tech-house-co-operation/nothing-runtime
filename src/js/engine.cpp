// js/engine.cpp
#include "engine.hpp"
#include "module_loader.hpp"
#include "globals.hpp"

#include <JavaScriptCore/JavaScript.h>

#include <iostream>
#include <cstring>
#include <cstdlib>

namespace nth::js {

namespace fs = std::filesystem;

std::string value_to_string(JSGlobalContextRef ctx, JSValueRef v) {
    if (!v) return "<null>";
    JSStringRef s = JSValueToStringCopy(ctx, v, nullptr);
    if (!s) return "<toString failed>";
    size_t len = JSStringGetMaximumUTF8CStringSize(s);
    std::string buf(len, '\0');
    size_t actual = JSStringGetUTF8CString(s, buf.data(), len);
    JSStringRelease(s);
    if (actual > 0) buf.resize(actual - 1); // includes trailing \0 in count
    else buf.clear();
    return buf;
}

JSValueRef make_error(JSGlobalContextRef ctx, const std::string& msg, JSValueRef* exception) {
    JSStringRef s = JSStringCreateWithUTF8CString(msg.c_str());
    JSValueRef err = JSValueMakeString(ctx, s);
    JSStringRelease(s);
    // Wrap in a real Error so .stack etc. work for catch sites.
    JSObjectRef global = JSContextGetGlobalObject(ctx);
    JSStringRef error_name = JSStringCreateWithUTF8CString("Error");
    JSValueRef error_ctor_v = JSObjectGetProperty(ctx, global, error_name, nullptr);
    JSStringRelease(error_name);
    if (error_ctor_v && JSValueIsObject(ctx, error_ctor_v)) {
        JSObjectRef error_ctor = const_cast<JSObjectRef>(error_ctor_v);
        JSValueRef args[] = { err };
        JSValueRef constructed = JSObjectCallAsConstructor(ctx, error_ctor, 1, args, nullptr);
        if (constructed) {
            if (exception) *exception = constructed;
            return constructed;
        }
    }
    if (exception) *exception = err;
    return err;
}

Engine::Engine() {
    // Create a context in a fresh group so it doesn't share state with any
    // other context (we only have one anyway).
    ctx_ = JSGlobalContextCreateInGroup(nullptr, nullptr);
    // Keep it alive even if no JS objects reference the global object.
    JSGlobalContextRetain(ctx_);
}

Engine::~Engine() {
    if (ctx_) JSGlobalContextRelease(ctx_);
}

void Engine::install_globals(bool http_enabled) {
    if (globals_installed_) return;
    globals_installed_ = true;
    globals::install(ctx_, http_enabled);
    module_loader::install(ctx_);
}

EvalResult Engine::eval_script(const std::string& source, const fs::path& file) {
    EvalResult r;
    install_globals(/*http_enabled=*/false); // plain-script path; main.cpp
                                              // should call eval_module if http
                                              // is on — but globals are
                                              // installed unconditionally so
                                              // both paths work.

    JSStringRef code = JSStringCreateWithUTF8CString(source.c_str());
    JSStringRef url = JSStringCreateWithUTF8CString(file.string().c_str());
    JSValueRef exc = nullptr;
    JSValueRef v = JSEvaluateScript(ctx_, code, /*thisObject=*/nullptr, url, 1, &exc);
    JSStringRelease(code);
    JSStringRelease(url);
    if (exc) {
        r.ok = false;
        r.exit_code = 1;
        // Build a helpful error string: message + (if available) stack
        JSStringRef msg_key = JSStringCreateWithUTF8CString("message");
        JSStringRef stack_key = JSStringCreateWithUTF8CString("stack");
        std::string msg;
        if (JSValueIsObject(ctx_, exc)) {
            JSObjectRef o = const_cast<JSObjectRef>(exc);
            JSValueRef m = JSObjectGetProperty(ctx_, o, msg_key, nullptr);
            if (m) msg = value_to_string(ctx_, m);
            JSValueRef stk = JSObjectGetProperty(ctx_, o, stack_key, nullptr);
            if (stk) {
                std::string s = value_to_string(ctx_, stk);
                if (!s.empty()) {
                    msg += "\n";
                    msg += s;
                }
            }
        } else {
            msg = value_to_string(ctx_, exc);
        }
        JSStringRelease(msg_key);
        JSStringRelease(stack_key);
        r.error_message = msg;
        return r;
    }
    (void)v;
    r.ok = true;
    r.exit_code = 0;
    return r;
}

EvalResult Engine::eval_module(const fs::path& file) {
    install_globals(/*http_enabled=*/true);
    module_root_ = file.parent_path();
    auto mlr = module_loader::eval_entry(ctx_, file);
    EvalResult r;
    r.ok = mlr.ok;
    r.error_message = std::move(mlr.error_message);
    r.exit_code = mlr.exit_code;
    return r;
}

void Engine::report_exception(const std::string& msg) {
    std::cerr << msg << "\n";
}

} // namespace nth::js
