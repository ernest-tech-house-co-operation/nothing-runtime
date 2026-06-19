// js/globals.hpp — installs JS global functions (console.log, fetch,
// Request/Response, Nth.serve) onto the JSC context.
#pragma once
#include <JavaScriptCore/JavaScript.h>

namespace nth::js::globals {

// Install globals. `http_enabled` controls whether the server-surface globals
// (fetch, Request, Response, Nth.serve, Nth.WebSocketPeer) are installed.
// `console.log` is always installed.
void install(JSGlobalContextRef ctx, bool http_enabled);

} // namespace nth::js::globals
