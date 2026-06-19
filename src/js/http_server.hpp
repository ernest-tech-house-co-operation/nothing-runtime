// js/http_server.hpp — Nth.serve() native primitive
#pragma once
#include <JavaScriptCore/JavaScript.h>

namespace nth::js::http_server {

// JS function: Nth.serve({ port, fetch(req)->Response, websocket? })
// Registers the server config and starts the C++ event loop AFTER the entry
// module finishes evaluating. Returns an object with a `stop()` method
// (no-op in v0.1 — the loop runs until Ctrl-C).
JSValueRef js_nth_serve(JSContextRef ctx, JSObjectRef function,
                        JSObjectRef thisObject, size_t argc,
                        const JSValueRef argv[], JSValueRef* exception);

// Run the registered server event loop. Blocks until interrupted (Ctrl-C)
// or until stop() is called. main.cpp calls this after the entry module
// finishes evaluating, if any server was registered.
//
// Returns the process exit code (0 = clean shutdown, non-zero on error).
int run_registered_servers(JSGlobalContextRef ctx);

} // namespace nth::js::http_server
