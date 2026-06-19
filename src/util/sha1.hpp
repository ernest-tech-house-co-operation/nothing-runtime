// util/sha1.hpp — public-domain SHA-1 + base64
//
// Used by server/websocket.cpp for the RFC 6455 Sec-WebSocket-Accept
// handshake. Vendoring SHA-1 here lets us drop the OpenSSL dependency,
// which simplifies the build on both Linux and Windows.
//
// SHA-1 implementation adapted from the public-domain reference by
// Steve Reid <sreid@sea-to-sky.net>, with modifications by
// James H. Brown <jbrown@burgoyne.com> and Saul Kravitz <saul@pseudo.eng.br>.
// Original header: "SHA-1 in C. By Steve Reid. 100% Public Domain."
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace nth::util {

// Compute SHA-1 over `data` and write the 20-byte digest to `out`.
void sha1(const void* data, size_t len, uint8_t out[20]);

// Compute SHA-1 over `data` and return the 20-byte digest as a 20-byte
// std::string (binary, not hex).
std::string sha1(const std::string& data);

// Base64-encode `len` bytes of `in`. Returns the encoded string (no
// trailing padding stripped — full = padding included).
std::string base64_encode(const uint8_t* in, size_t len);

} // namespace nth::util
