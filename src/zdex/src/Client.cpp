#include "zdex/Client.h"
#include "zdex/Http.h"
#include "zdex/Json.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

namespace zircon::zdex {
namespace {

constexpr int kMaxAttempts = 5;

// Longest Retry-After worth sitting through. Past this it is a quota, not a burst.
constexpr int kMaxHonouredWait = 120;

void Sleep(int seconds) {
    if (seconds > 0) std::this_thread::sleep_for(std::chrono::seconds(seconds));
}

Outcome OutcomeFor(long status, const std::string& code) {
    if (status == 401) return Outcome::Auth;
    if (status == 403 && code == "unverified") return Outcome::Auth;
    if (status == 400 || status == 404 || status == 409 || status == 410 || status == 422)
        return Outcome::Usage;
    if (status == 403) return Outcome::Usage;    // csrf and friends: not a key problem
    return Outcome::Network;
}

Result FromBody(long status, const JsonValue& body) {
    Result result;
    result.status = status;
    result.ok = status >= 200 && status < 300 && body.Bool("ok", status < 300);
    if (result.ok) return result;

    result.error = body.Str("error");
    result.message = body.Str("message");
    if (result.message.empty()) {
        result.message = result.error.empty()
            ? "the server answered HTTP " + std::to_string(status)
            : result.error;
    }
    result.outcome = OutcomeFor(status, result.error);
    return result;
}

} // namespace

std::string UserAgent(std::string_view version) {
    return "Zircon/" + std::string(version) + " (+https://zlogic.eu/zircon/)";
}

Result Client::Send(const std::string& method, const std::string& url, const std::string& body,
                    const std::string& content_type, JsonValue& out, int timeout_seconds) {
    int wait = 1;

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        HttpRequest request;
        request.method = method;
        request.url = url;
        request.bearer = config_.api_key;
        request.content_type = content_type;
        request.body = body;
        request.user_agent = user_agent_;
        request.timeout_seconds = timeout_seconds;
        request.on_upload = on_progress;

        const HttpResponse response = HttpSend(request);

        if (response.status == 0) {
            if (response.transport_error == "cancelled") {
                Result cancelled;
                cancelled.outcome = Outcome::Usage;
                cancelled.error = "cancelled";
                cancelled.message = "cancelled";
                return cancelled;
            }
            if (attempt == kMaxAttempts) {
                Result failed;
                failed.outcome = Outcome::Network;
                failed.error = "transport";
                failed.message = response.transport_error;
                return failed;
            }
            if (on_retry) on_retry(response.transport_error, wait);
            Sleep(wait);
            wait = wait < 15 ? wait * 2 : 15;
            continue;
        }

        std::string parse_error;
        const bool parsed = ParseJson(response.body, out, parse_error);

        // Cloudflare's browser check answers 403 with `error code: 1010` in plain text.
        // Saying "not JSON" here would send the user hunting in the wrong place.
        if (!parsed) {
            Result failed;
            failed.status = response.status;
            failed.outcome = Outcome::Network;
            failed.error = "not_json";
            failed.message = response.status == 403
                ? "blocked before reaching Zdex (Cloudflare). This is normally a missing or "
                  "generic User-Agent; zircon sends one, so if you see this behind a proxy or "
                  "VPN, try without it."
                : "the server answered HTTP " + std::to_string(response.status) +
                  " with something that is not JSON";
            return failed;
        }

        if (response.status == 429) {
            const int hold = response.retry_after > 0 ? response.retry_after : wait;

            // Waiting out a short burst limit is helpful. Waiting out the 8-per-hour
            // upload quota is not: Retry-After comes back as most of an hour, and a CLI
            // that silently blocks for 54 minutes looks like it has hung. Hand the number
            // back and let the user decide.
            if (hold <= kMaxHonouredWait && attempt < kMaxAttempts) {
                if (on_retry) on_retry("rate limited", hold);
                Sleep(hold);
                wait = wait < 15 ? wait * 2 : 15;
                continue;
            }

            // The server's own message already names the wait ("Try again in 33 min"),
            // and the handoff asks for those printed verbatim. Adding our own sentence
            // just says it twice.
            Result limited = FromBody(response.status, out);
            limited.outcome = Outcome::Network;
            return limited;
        }

        // 5xx is worth one more try; everything else is the server's considered answer.
        if (response.status >= 500 && attempt < kMaxAttempts) {
            if (on_retry) on_retry("server error " + std::to_string(response.status), wait);
            Sleep(wait);
            wait = wait < 15 ? wait * 2 : 15;
            continue;
        }

        Result result = FromBody(response.status, out);
        if (response.status == 429) result.outcome = Outcome::Network;
        return result;
    }

    Result failed;
    failed.outcome = Outcome::Network;
    failed.error = "exhausted";
    failed.message = "gave up after " + std::to_string(kMaxAttempts) + " attempts";
    return failed;
}

Result Client::CheckKey() {
    JsonValue body;
    const Result result = Send("GET",
        config_.ApiBase() + "/upload/status/00000000000000000000000000000000",
        {}, {}, body, 30);

    // 404 not_found is the pass: the route ran, the key was accepted, the id is simply not
    // a real upload. Anything 2xx would be a surprise but is equally fine.
    if (result.status == 404) return Result::Success();
    if (result.ok) return Result::Success();
    return result;
}

Result Client::Init(const std::string& filename, std::uint64_t size, const std::string& game,
                    const std::string& label, const std::string& notes, InitResult& out) {
    std::string body = "{\"filename\":\"" + JsonEscape(filename) +
                       "\",\"size\":" + std::to_string(size) +
                       ",\"game\":\"" + JsonEscape(game) +
                       "\",\"label\":\"" + JsonEscape(label) + "\"";
    if (!notes.empty()) body += ",\"notes\":\"" + JsonEscape(notes) + "\"";
    body += "}";

    JsonValue response;
    const Result result = Send("POST", config_.ApiBase() + "/upload/init", body,
                               "application/json", response, 60);
    if (!result.ok) return result;

    out.upload_id = response.Str("upload_id");
    out.chunk_bytes = static_cast<std::uint64_t>(response.Int("chunk_bytes"));
    out.chunk_count = static_cast<int>(response.Int("chunk_count"));
    out.received = response.IntArray("received");

    if (out.upload_id.empty() || out.chunk_bytes == 0) {
        Result bad;
        bad.outcome = Outcome::Network;
        bad.error = "bad_init";
        bad.message = "the server accepted the upload but did not say where to send it";
        return bad;
    }
    return result;
}

Result Client::UploadStatus(const std::string& upload_id, InitResult& out) {
    JsonValue response;
    const Result result = Send("GET", config_.ApiBase() + "/upload/status/" + upload_id,
                               {}, {}, response, 30);
    if (!result.ok) return result;

    out.upload_id = upload_id;
    out.chunk_bytes = static_cast<std::uint64_t>(response.Int("chunk_bytes", 0));
    out.chunk_count = static_cast<int>(response.Int("chunk_count", 0));
    out.received = response.IntArray("received");

    if (response.Str("status") != "open") {
        Result closed;
        closed.outcome = Outcome::Usage;
        closed.error = "closed";
        closed.message = "that upload session is no longer open";
        return closed;
    }
    return result;
}

Result Client::SendChunk(const std::string& upload_id, int index, std::string_view bytes) {
    const std::string url = config_.ApiBase() + "/upload/chunk?upload_id=" +
                            UrlEncode(upload_id) + "&index=" + std::to_string(index);

    JsonValue response;
    // Generous: an 8 MB chunk on a slow line is minutes, and a timeout here restarts work
    // the server has already accepted.
    return Send("POST", url, std::string(bytes), "application/octet-stream", response, 300);
}

Result Client::Finish(const std::string& upload_id, FinishResult& out) {
    const std::string body = "{\"upload_id\":\"" + JsonEscape(upload_id) + "\"}";

    JsonValue response;
    // The server hashes and stores the file inside this call; the handoff asks for five
    // minutes of patience.
    const Result result = Send("POST", config_.ApiBase() + "/upload/finish", body,
                               "application/json", response, 300);

    if (result.ok) {
        out.dump_id = response.Int("dump_id");
        out.url = response.Str("url");
        out.status = response.Str("status");
        return result;
    }

    // A duplicate is not a failure: the dump is on Zdex, which is what the user asked for.
    // The server names the existing one in the same shape.
    if (result.error == "rejected" || result.error == "duplicate") {
        const std::int64_t existing = response.Int("dump_id");
        if (existing > 0) {
            out.dump_id = existing;
            out.url = response.Str("url");
            if (out.url.empty())
                out.url = config_.base_url + "/d/" + std::to_string(existing);
            out.status = response.Str("status", "ready");
            out.duplicate = true;
            Result ok = Result::Success();
            ok.message = result.message;
            return ok;
        }
    }
    return result;
}

Result Client::Status(std::int64_t dump_id, DumpStatus& out) {
    JsonValue response;
    const Result result = Send("GET",
        config_.ApiBase() + "/dump/" + std::to_string(dump_id) + "/status", {}, {},
        response, 30);
    if (!result.ok) return result;

    out.id = response.Int("id", dump_id);
    out.status = response.Str("status");
    out.progress = static_cast<int>(response.Int("progress"));
    out.phase_label = response.Str("phase_label");
    out.error = response.Str("error");
    return result;
}

Result Client::Download(std::int64_t dump_id, const std::string& kind,
                        const std::string& out_path) {
    HttpRequest request;
    request.method = "GET";
    request.url = config_.base_url + "/d/" + std::to_string(dump_id) + "/download/" + kind;
    request.user_agent = user_agent_;
    request.timeout_seconds = 300;
    // No bearer: downloads of published dumps are open, and sending a key to a redirect
    // target we do not control is worse than not sending one.

    const HttpResponse response = HttpSend(request);
    if (response.status == 0) {
        Result failed;
        failed.outcome = Outcome::Network;
        failed.error = "transport";
        failed.message = response.transport_error;
        return failed;
    }
    if (!response.ok()) {
        JsonValue body;
        std::string ignored;
        ParseJson(response.body, body, ignored);
        return FromBody(response.status, body);
    }

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        Result failed;
        failed.outcome = Outcome::Usage;
        failed.error = "cannot_write";
        failed.message = "cannot write " + out_path;
        return failed;
    }
    out.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
    if (!out) {
        Result failed;
        failed.outcome = Outcome::Usage;
        failed.error = "cannot_write";
        failed.message = "write failed for " + out_path;
        return failed;
    }
    return Result::Success();
}

} // namespace zircon::zdex
