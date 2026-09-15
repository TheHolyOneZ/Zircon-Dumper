#pragma once

// HTTPS over WinHTTP. Ships with Windows, so it adds no dependency and no DLL to
// redistribute - which matters for a tool whose whole release is three files.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace zircon::zdex {

struct HttpResponse {
    long status{0};             // 0 means the request never reached the server
    std::string body;
    std::string transport_error; // populated only when status == 0
    int retry_after{0};          // seconds, from the Retry-After header on a 429

    bool ok() const { return status >= 200 && status < 300; }
};

struct HttpRequest {
    std::string method{"GET"};
    std::string url;
    std::string bearer;                 // sent as Authorization: Bearer <...> when set
    std::string content_type;
    std::string body;

    // Zdex sits behind Cloudflare, whose browser-integrity check answers a plain-text 403
    // "error code: 1010" to generic library agents. Every request must name itself.
    std::string user_agent;

    int timeout_seconds{60};

    // Called with (sent, total) while a body is uploading. Return false to abort.
    std::function<bool(std::uint64_t, std::uint64_t)> on_upload;
};

HttpResponse HttpSend(const HttpRequest& request);

// Percent-encodes a query-string value.
std::string UrlEncode(std::string_view text);

} // namespace zircon::zdex
