// js/fetch.hpp — outbound fetch() + Request/Response constructors
#pragma once
#include <JavaScriptCore/JavaScript.h>

namespace nth::js::fetch {

// Construct the JS `Request` constructor object. Returned JS objects have
// properties: method, url, headers, body.
JSObjectRef make_request_constructor(JSGlobalContextRef ctx);

// Construct the JS `Response` constructor object. Calling new Response(body, init)
// returns an object with: status, statusText, headers, body, plus methods
// json() and text() returning Promises.
JSObjectRef make_response_constructor(JSGlobalContextRef ctx);

// Implementation of the global `fetch(url, options)` function. Returns a
// Promise resolving to a Response.
JSValueRef js_fetch(JSContextRef ctx, JSObjectRef function,
                    JSObjectRef thisObject, size_t argc,
                    const JSValueRef argv[], JSValueRef* exception);

} // namespace nth::js::fetch
