#include "zdex/Http.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <memory>

#pragma comment(lib, "winhttp.lib")

namespace zircon::zdex {
namespace {

struct HandleCloser {
    void operator()(HINTERNET h) const noexcept { if (h) ::WinHttpCloseHandle(h); }
};
using Handle = std::unique_ptr<std::remove_pointer_t<HINTERNET>, HandleCloser>;

std::wstring Widen(std::string_view text) {
    if (text.empty()) return {};
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          out.data(), needed);
    return out;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()),
                                             nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          out.data(), needed, nullptr, nullptr);
    return out;
}

struct ParsedUrl {
    bool https{true};
    std::wstring host;
    int port{443};
    std::wstring path;
    bool valid{false};
};

ParsedUrl ParseUrl(const std::string& url) {
    ParsedUrl out;
    const std::wstring wide = Widen(url);

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    parts.dwSchemeLength = static_cast<DWORD>(-1);

    if (!::WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts))
        return out;

    out.https = parts.nScheme == INTERNET_SCHEME_HTTPS;
    out.port = parts.nPort;
    out.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    out.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) out.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (out.path.empty()) out.path = L"/";
    out.valid = true;
    return out;
}

std::string LastError(std::string_view stage) {
    const DWORD code = ::GetLastError();
    char* text = nullptr;

    // WinHTTP's own codes live in winhttp.dll's message table, not the system one.
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS;
    ::FormatMessageA(flags, ::GetModuleHandleW(L"winhttp.dll"), code, 0,
                     reinterpret_cast<char*>(&text), 0, nullptr);

    std::string message = text ? std::string(text) : std::string();
    if (text) ::LocalFree(text);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
        message.pop_back();

    if (message.empty()) message = "error " + std::to_string(code);
    return std::string(stage) + ": " + message;
}

int RetryAfterSeconds(HINTERNET request) {
    wchar_t buffer[64] = {};
    DWORD size = sizeof(buffer);
    if (!::WinHttpQueryHeaders(request, WINHTTP_QUERY_RETRY_AFTER, WINHTTP_HEADER_NAME_BY_INDEX,
                               buffer, &size, WINHTTP_NO_HEADER_INDEX))
        return 0;
    const int seconds = ::_wtoi(buffer);
    return seconds > 0 ? seconds : 0;
}

} // namespace

std::string UrlEncode(std::string_view text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3 / 2);
    for (const unsigned char c : text) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

HttpResponse HttpSend(const HttpRequest& request) {
    HttpResponse response;

    const ParsedUrl url = ParseUrl(request.url);
    if (!url.valid) {
        response.transport_error = "not a usable URL: " + request.url;
        return response;
    }

    Handle session(::WinHttpOpen(Widen(request.user_agent).c_str(),
                                 WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        response.transport_error = LastError("cannot start WinHTTP");
        return response;
    }

    const DWORD ms = static_cast<DWORD>(request.timeout_seconds) * 1000;
    ::WinHttpSetTimeouts(session.get(), static_cast<int>(ms), static_cast<int>(ms),
                         static_cast<int>(ms), static_cast<int>(ms));

    Handle connection(::WinHttpConnect(session.get(), url.host.c_str(),
                                       static_cast<INTERNET_PORT>(url.port), 0));
    if (!connection) {
        response.transport_error = LastError("cannot connect to " + Narrow(url.host));
        return response;
    }

    Handle http(::WinHttpOpenRequest(connection.get(), Widen(request.method).c_str(),
                                     url.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     url.https ? WINHTTP_FLAG_SECURE : 0));
    if (!http) {
        response.transport_error = LastError("cannot open the request");
        return response;
    }

    std::wstring headers;
    if (!request.bearer.empty())
        headers += L"Authorization: Bearer " + Widen(request.bearer) + L"\r\n";
    if (!request.content_type.empty())
        headers += L"Content-Type: " + Widen(request.content_type) + L"\r\n";
    headers += L"Accept: application/json\r\n";

    // The body goes out in slices so a stalled 8 MB chunk can still report progress and be
    // cancelled, rather than sitting in one opaque WinHttpSendRequest call.
    const DWORD total = static_cast<DWORD>(request.body.size());
    if (!::WinHttpSendRequest(http.get(),
                              headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                              static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, total, 0)) {
        response.transport_error = LastError("cannot send the request");
        return response;
    }

    if (total > 0) {
        constexpr DWORD kSlice = 256u * 1024u;
        DWORD sent = 0;
        while (sent < total) {
            const DWORD take = std::min(kSlice, total - sent);
            DWORD wrote = 0;
            if (!::WinHttpWriteData(http.get(), request.body.data() + sent, take, &wrote)) {
                response.transport_error = LastError("upload interrupted");
                return response;
            }
            sent += wrote;
            if (request.on_upload && !request.on_upload(sent, total)) {
                response.transport_error = "cancelled";
                return response;
            }
        }
    }

    if (!::WinHttpReceiveResponse(http.get(), nullptr)) {
        response.transport_error = LastError("no response");
        return response;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    ::WinHttpQueryHeaders(http.get(),
                          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                          WINHTTP_NO_HEADER_INDEX);
    response.status = static_cast<long>(status);
    if (status == 429) response.retry_after = RetryAfterSeconds(http.get());

    for (;;) {
        DWORD available = 0;
        if (!::WinHttpQueryDataAvailable(http.get(), &available)) {
            response.transport_error = LastError("reading the response failed");
            response.status = 0;
            return response;
        }
        if (available == 0) break;

        const std::size_t at = response.body.size();
        response.body.resize(at + available);
        DWORD read = 0;
        if (!::WinHttpReadData(http.get(), response.body.data() + at, available, &read)) {
            response.transport_error = LastError("reading the response failed");
            response.status = 0;
            return response;
        }
        response.body.resize(at + read);
        if (read == 0) break;
    }

    return response;
}

} // namespace zircon::zdex
