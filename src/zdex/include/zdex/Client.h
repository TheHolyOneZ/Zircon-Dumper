#pragma once

// The Zdex publish protocol: init -> chunk -> finish -> poll.
//
// Every call returns a Result rather than throwing, because every failure here has a
// message the server wrote for the user and the CLI prints those verbatim.

#include "zdex/Config.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace zircon::zdex {

// Exit codes the CLI maps onto, per the integration handoff.
enum class Outcome {
    Ok = 0,
    Usage = 1,      // validation, a rejected file, anything the user typed wrong
    Auth = 2,       // no key, bad key, unverified account
    Network = 3,    // transport, 5xx, rate limits that outlast our patience
};

struct Result {
    bool ok{false};
    Outcome outcome{Outcome::Ok};
    std::string error;      // short code from the server, e.g. "rate_limited"
    std::string message;    // human sentence, safe to print as-is
    long status{0};

    static Result Success() { return Result{true, Outcome::Ok, {}, {}, 200}; }
};

struct InitResult {
    std::string upload_id;
    std::uint64_t chunk_bytes{0};
    int chunk_count{0};
    std::vector<std::int64_t> received;
};

struct FinishResult {
    std::int64_t dump_id{0};
    std::string url;
    std::string status;
    bool duplicate{false};   // the dump was already on Zdex; url points at the existing one
};

struct DumpStatus {
    std::int64_t id{0};
    std::string status;      // queued | importing | pending | ready | failed
    int progress{0};
    std::string phase_label;
    std::string error;

    bool settled() const { return status == "ready" || status == "pending" || status == "failed"; }
};

class Client {
public:
    Client(Config config, std::string user_agent)
        : config_(std::move(config)), user_agent_(std::move(user_agent)) {}

    const Config& config() const { return config_; }

    // Cheapest call that proves a key works: a status lookup for an id that cannot exist.
    // 404 means the key is good. Costs no upload quota, which `zircon login` cares about.
    Result CheckKey();

    Result Init(const std::string& filename, std::uint64_t size, const std::string& game,
                const std::string& label, const std::string& notes, InitResult& out);

    // Resumes an existing session. Returns ok=false with error "closed" when the server
    // has forgotten it, which means start again from Init.
    Result UploadStatus(const std::string& upload_id, InitResult& out);

    Result SendChunk(const std::string& upload_id, int index, std::string_view bytes);

    Result Finish(const std::string& upload_id, FinishResult& out);

    Result Status(std::int64_t dump_id, DumpStatus& out);

    // Plain GET, no auth, follows redirects. `kind` is usmap | sdk | json.
    Result Download(std::int64_t dump_id, const std::string& kind, const std::string& out_path);

    // Set to be told about retries, so the CLI can say why it is waiting.
    std::function<void(const std::string& reason, int seconds)> on_retry;

    // Progress within the current chunk upload.
    std::function<bool(std::uint64_t, std::uint64_t)> on_progress;

private:
    Result Send(const std::string& method, const std::string& url, const std::string& body,
                const std::string& content_type, class JsonValue& out, int timeout_seconds);

    Config config_;
    std::string user_agent_;
};

// "Zircon/0.4.0 (+https://zlogic.eu/zircon/)" — Cloudflare answers a plain-text 403 to
// generic agents, so this is not cosmetic.
std::string UserAgent(std::string_view version);

} // namespace zircon::zdex
