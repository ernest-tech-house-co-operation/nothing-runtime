// server/http.hpp — minimal HTTP/1.1 request parser & response serializer
#pragma once
#include <string>
#include <vector>
#include <utility>

namespace nth::server {

struct HttpRequest {
    std::string method;
    std::string path;        // path + query, no scheme/host
    std::string version;     // "HTTP/1.1"
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string status_text;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

// Parse the header block (everything up to the blank line) of an HTTP
// request. Does NOT consume the body. Returns false on malformed input.
bool parse_request(const std::string& header_block, HttpRequest& out);

// Default status text for a code (e.g. 200 → "OK").
const char* status_text(int code);

} // namespace nth::server
