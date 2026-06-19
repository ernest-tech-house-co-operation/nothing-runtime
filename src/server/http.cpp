// server/http.cpp
#include "http.hpp"
#include "../util/strings.hpp"

#include <sstream>

namespace nth::server {

bool parse_request(const std::string& header_block, HttpRequest& out) {
    // First line: METHOD PATH VERSION
    auto first_eol = header_block.find("\r\n");
    if (first_eol == std::string::npos) return false;
    std::string line = header_block.substr(0, first_eol);
    auto sp1 = line.find(' ');
    if (sp1 == std::string::npos) return false;
    auto sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;
    out.method = line.substr(0, sp1);
    out.path = line.substr(sp1 + 1, sp2 - sp1 - 1);
    out.version = line.substr(sp2 + 1);

    size_t pos = first_eol + 2;
    while (pos < header_block.size()) {
        auto eol = header_block.find("\r\n", pos);
        std::string hline = (eol == std::string::npos)
            ? header_block.substr(pos) : header_block.substr(pos, eol - pos);
        if (hline.empty()) break;
        auto c = hline.find(':');
        if (c != std::string::npos) {
            std::string k = hline.substr(0, c);
            std::string v = hline.substr(c + 1);
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
            out.headers.emplace_back(std::move(k), std::move(v));
        }
        if (eol == std::string::npos) break;
        pos = eol + 2;
    }
    return true;
}

const char* status_text(int code) {
    switch (code) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default:  return "OK";
    }
}

} // namespace nth::server
