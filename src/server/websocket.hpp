// server/websocket.hpp — RFC 6455 handshake + minimal frame I/O
#pragma once
#include <JavaScriptCore/JavaScript.h>
#include <string>
#include "../util/net.hpp"

namespace nth::server {

// Compute the Sec-WebSocket-Accept value for the given Sec-WebSocket-Key.
std::string compute_accept(const std::string& key);

// Perform the WebSocket upgrade handshake on `fd` using the given
// `sec_websocket_key`, then enter a read loop dispatching frames to the
// JS `websocket` handler object (which should have open/message/close
// callbacks). Returns when the client disconnects or sends a close frame.
//
// `handler` is a JS object with optional callbacks:
//   handler.open(peer)          — called once after handshake succeeds
//   handler.message(peer, data) — called for each text/binary message
//   handler.close(peer)         — called when the connection closes
//
// `peer` is a JS object with .send(data) and .close() methods.
void websocket_handshake_and_loop(JSGlobalContextRef ctx, util::net::socket_t fd,
                                  const std::string& sec_websocket_key,
                                  JSObjectRef handler);

} // namespace nth::server
