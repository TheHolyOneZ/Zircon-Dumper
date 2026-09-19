#include "Publish.h"

#include "ir/Json.h"

#include "core/Log.h"
#include "core/Term.h"
#include "zdex/Client.h"
#include "zdex/Config.h"
#include "zdex/Gzip.h"
#include "zdex/Upload.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <io.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <thread>

using namespace zircon::core;
using namespace zircon::core::term;

namespace zircon::app {
namespace {

constexpr const char* kVersion = ZIRCON_VERSION;

void Field(std::string_view label, std::string_view value) {
    std::printf("%.*s%-16s%.*s %s\n", static_cast<int>(Dim().size()), Dim().data(),
                std::string(label).c_str(), static_cast<int>(Reset().size()), Reset().data(),
                std::string(value).c_str());
}

void FieldStrong(std::string_view label, std::string_view value) {
    std::printf("%.*s%-16s%.*s %.*s%s%.*s\n",
                static_cast<int>(Dim().size()), Dim().data(), std::string(label).c_str(),
                static_cast<int>(Reset().size()), Reset().data(),
                static_cast<int>(Bold().size()), Bold().data(), std::string(value).c_str(),
                static_cast<int>(Reset().size()), Reset().data());
}

std::string Human(std::uint64_t bytes) {
    if (bytes >= (1ull << 30)) return std::format("{:.1f} GB", bytes / 1073741824.0);
    if (bytes >= (1ull << 20)) return std::format("{:.1f} MB", bytes / 1048576.0);
    if (bytes >= 1024)         return std::format("{:.1f} KB", bytes / 1024.0);
    return std::format("{} B", bytes);
}

// Overwrites one line rather than scrolling. Falls back to nothing when output is
// redirected, so a log file does not fill up with carriage returns.
class Progress {
public:
    // Only when stdout is a console. Redirected to a file, a carriage-return progress bar
    // is 200 lines of noise with the answer buried in it.
    explicit Progress(bool wanted)
        : enabled_(wanted && ::_isatty(::_fileno(stdout)) != 0) {}

    void Show(std::string_view text) {
        if (!enabled_) return;
        std::printf("\r%-72s", std::string(text).c_str());
        std::fflush(stdout);
        dirty_ = true;
    }

    void Clear() {
        if (!enabled_ || !dirty_) return;
        std::printf("\r%-72s\r", "");
        std::fflush(stdout);
        dirty_ = false;
    }

    ~Progress() { Clear(); }

private:
    bool enabled_;
    bool dirty_{false};
};

int ExitFor(zdex::Outcome outcome) { return static_cast<int>(outcome); }

// The server writes its messages for users; printing anything else in front of them only
// gets in the way.
int Fail(const zdex::Result& result) {
    std::fflush(stdout);
    LogError("{}", result.message.empty() ? result.error : result.message);
    if (result.outcome == zdex::Outcome::Auth)
        LogError("run 'zircon login' with a key from {}/account#apikeys",
                 zdex::LoadConfig().base_url);
    return ExitFor(result.outcome);
}

std::string ReadHidden(std::string_view prompt) {
    std::printf("%s", std::string(prompt).c_str());
    std::fflush(stdout);

    const HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool console = ::GetConsoleMode(in, &mode) != 0;
    if (console) ::SetConsoleMode(in, mode & ~ENABLE_ECHO_INPUT);

    std::string line;
    std::getline(std::cin, line);

    if (console) {
        ::SetConsoleMode(in, mode);
        std::printf("\n");
    }

    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    // Pasting from a web page picks up spaces surprisingly often.
    while (!line.empty() && line.front() == ' ') line.erase(line.begin());
    while (!line.empty() && line.back() == ' ') line.pop_back();
    return line;
}

bool AskYesNo(std::string_view question) {
    std::printf("%s [y/N] ", std::string(question).c_str());
    std::fflush(stdout);
    std::string line;
    std::getline(std::cin, line);
    return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
}

bool LooksLikeKey(std::string_view key) {
    // zdx_ plus 40 URL-safe base64 characters. Checked so an obvious paste error is caught
    // before a round trip, never to decide whether a key is valid - only the server does.
    if (key.size() != 44 || key.rfind("zdx_", 0) != 0) return false;
    return std::all_of(key.begin() + 4, key.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

zdex::Client MakeClient(const zdex::Config& config) {
    zdex::Client client(config, zdex::UserAgent(kVersion));
    client.on_retry = [](const std::string& reason, int seconds) {
        LogWarn("{}; retrying in {}s", reason, seconds);
    };
    return client;
}

std::string ReadFile(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "cannot open " + path; return {}; }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void OpenInBrowser(const std::string& url) {
    ::ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace

std::string GameNameFromProcess(std::string_view process) {
    std::string name(process);
    for (const char* suffix : {"-Win64-Shipping.exe", "-Win64-Test.exe", "-WinGDK-Shipping.exe",
                               "-Win32-Shipping.exe", ".exe"}) {
        if (EndsWith(name, suffix)) {
            name.resize(name.size() - std::strlen(suffix));
            break;
        }
    }
    return name;
}

std::string LabelFromTimestamp(std::string_view created_utc) {
    // "2026-09-15T10:02:49Z" -> "2026-09-15". Anything unexpected falls back to the whole
    // string, which the server will validate.
    if (created_utc.size() >= 10) return std::string(created_utc.substr(0, 10));
    return std::string(created_utc);
}

int CommandLogin(std::string_view key_argument) {
    zdex::Config config = zdex::LoadConfig();

    std::string key(key_argument);
    if (key.empty()) {
        std::printf("Zdex stores an API key, not an account password.\n");
        std::printf("Create one at %s/account#apikeys - it is shown once.\n\n",
                    config.base_url.c_str());
        key = ReadHidden("API key: ");
    }

    if (key.empty()) {
        LogError("no key given");
        return 1;
    }
    if (!LooksLikeKey(key)) {
        LogError("that does not look like a Zdex key (expected zdx_ followed by 40 characters)");
        return 1;
    }

    config.api_key = key;
    zdex::Client client = MakeClient(config);

    // Validated against the server, not just its shape, so a revoked key is caught now
    // rather than in the middle of a 300 MB upload.
    const zdex::Result check = client.CheckKey();
    if (!check.ok) return Fail(check);

    if (config.from_environment) {
        LogWarn("ZDEX_API_KEY is set and overrides the stored key for this shell");
    }

    std::string error;
    // A key change means the terms confirmation belongs to a different account.
    if (config.terms_accepted_for != config.KeyHint()) config.terms_accepted_for.clear();
    if (!zdex::SaveConfig(config, error)) {
        LogError("{}", error);
        return 1;
    }

    FieldStrong("signed in", config.KeyHint());
    Field("stored in", zdex::ConfigPath());
    Field("zdex", config.base_url);
    return 0;
}

int CommandLogout() {
    const zdex::Config config = zdex::LoadConfig();
    std::string error;
    if (!zdex::ClearConfig(error)) {
        LogError("{}", error);
        return 1;
    }
    if (config.from_environment)
        LogWarn("ZDEX_API_KEY is still set in this environment and will keep being used");
    FieldStrong("signed out", "the stored key is gone");
    return 0;
}

std::string PublishRefusal(std::string_view runtime) {
    if (runtime.empty() || runtime == "unreal" || runtime == "il2cpp") return {};
    return std::format("this dump reports its runtime as '{}', which Zdex does not index",
                       runtime);
}

std::string PublishRefusalForFile(std::string_view path) {
    const auto header = ir::ReadJsonHeaderFile(path);
    // An unreadable header is not the gate's business. Whatever is wrong with the file, the
    // upload path reports it better than a refusal phrased as being about runtimes would.
    if (!header) return {};
    return PublishRefusal(header.value().runtime);
}

int CommandPublish(const PublishOptions& options) {
    zdex::Config config = zdex::LoadConfig();
    if (!config.HasKey()) {
        LogError("no Zdex key stored; run 'zircon login' first");
        return ExitFor(zdex::Outcome::Auth);
    }

    std::error_code ec;
    if (!std::filesystem::exists(options.path, ec)) {
        LogError("no such file: {}", options.path);
        return 1;
    }
    if (options.game.empty() || options.label.empty()) {
        LogError("publish needs --game and --label");
        return 1;
    }

    if (const auto refusal = PublishRefusalForFile(options.path); !refusal.empty()) {
        LogError("{}", refusal);
        return ExitFor(zdex::Outcome::Usage);
    }

    // --- everything that can be checked without sending anything ---------------------
    if (options.dry_run) {
        const auto size = std::filesystem::file_size(options.path, ec);
        FieldStrong("dry run", "nothing will be sent");
        Field("file", options.path);
        Field("size", ec ? std::string("unknown") : Human(size));
        Field("game", options.game);
        Field("label", options.label);
        if (!options.notes.empty()) Field("notes", options.notes);
        Field("server", config.base_url);
        Field("key", config.KeyHint());
        std::printf("\nIt would be accepted. Drop --dry-run to send it.\n");
        return 0;
    }

    // --- the one-time confirmation ---------------------------------------------------
    if (!options.assume_yes && config.terms_accepted_for != config.KeyHint()) {
        std::printf(
            "Publishing puts this dump on %s under your account. You confirm you made it\n"
            "yourself with Zircon, it contains reflection metadata only, and you are\n"
            "responsible for it (%s/terms#uploads).\n",
            config.base_url.c_str(), config.base_url.c_str());
        if (!AskYesNo("Continue?")) {
            LogError("cancelled");
            return 1;
        }
        config.terms_accepted_for = config.KeyHint();
        std::string save_error;
        if (!config.from_environment) zdex::SaveConfig(config, save_error);
    }

    // --- hand it to the shared flow --------------------------------------------------
    zdex::Client client = MakeClient(config);

    zdex::UploadRequest request;
    request.path  = options.path;
    request.game  = options.game;
    request.label = options.label;
    request.notes = options.notes;
    request.wait  = options.wait;

    Progress bar(!options.json_output);

    zdex::UploadHooks hooks;
    hooks.progress = [&](zdex::UploadPhase phase, std::uint64_t done, std::uint64_t total,
                         std::string_view note) {
        switch (phase) {
        case zdex::UploadPhase::Compressing:
            if (total) bar.Show(std::format("compressing  {}%  {} / {}",
                                            done * 100 / total, Human(done), Human(total)));
            break;
        case zdex::UploadPhase::Resuming:
            break;
        case zdex::UploadPhase::Uploading:
            bar.Show(std::format("uploading    {}%  {} / {}   {}",
                                 total ? done * 100 / total : 100, Human(done), Human(total),
                                 std::string(note)));
            break;
        case zdex::UploadPhase::Finishing:
            bar.Show("finishing");
            break;
        case zdex::UploadPhase::Indexing:
            bar.Show(std::format("indexing     {}%  {}", done, std::string(note)));
            break;
        }
    };

    const zdex::UploadReport report = zdex::Upload(client, request, hooks);
    bar.Clear();

    if (!options.json_output) {
        if (report.compressed)
            Field("compressed", std::format("{} -> {} ({:.1f}x)", Human(report.stats.raw),
                                            Human(report.stats.compressed),
                                            report.stats.ratio()));
        if (report.resumed_chunks >= 0)
            Field("resuming", std::format("{} of {} chunks already sent",
                                          report.resumed_chunks, report.chunk_count));
    }

    if (!report.ok) {
        zdex::Result failure;
        failure.outcome = report.outcome;
        failure.error   = report.error;
        failure.message = report.message;
        return Fail(failure);
    }

    if (options.json_output) {
        std::printf("{\"dump_id\": %lld, \"url\": \"%s\", \"status\": \"%s\", "
                    "\"duplicate\": %s}\n",
                    static_cast<long long>(report.dump_id), report.url.c_str(),
                    report.status.c_str(), report.duplicate ? "true" : "false");
    } else {
        if (report.duplicate) {
            Field("already on zdex", report.message.empty() ? "an identical dump exists"
                                                            : report.message);
        }
        FieldStrong("url", report.url);
        if (!report.status.empty()) Field("status", report.status);
        if (report.status == "pending")
            Field("note", "waiting for review before it appears publicly");
        if (report.status == "failed" && !report.status_error.empty()) {
            LogError("{}", report.status_error);
            return ExitFor(zdex::Outcome::Usage);
        }
    }

    if (options.open_browser && !report.url.empty()) OpenInBrowser(report.url);
    return 0;
}

int CommandFetch(std::int64_t dump_id, std::string_view kind, std::string_view out_path) {
    if (dump_id <= 0) {
        LogError("fetch needs a dump id, e.g. zircon fetch 42 --usmap -o game.usmap");
        return 1;
    }
    if (kind != "usmap" && kind != "sdk" && kind != "json") {
        LogError("fetch takes --usmap, --sdk or --json");
        return 1;
    }

    const zdex::Config config = zdex::LoadConfig();
    zdex::Client client = MakeClient(config);

    std::string target(out_path);
    if (target.empty()) {
        // Named after what it is, since Content-Disposition is not read back here.
        target = std::format("zdex-{}-{}", dump_id, kind);
        if (kind == "usmap") target += ".usmap";
        else if (kind == "sdk") target += ".zip";
        else target += ".json";
    }

    const zdex::Result result = client.Download(dump_id, std::string(kind), target);
    if (!result.ok) return Fail(result);

    std::error_code ec;
    FieldStrong("saved", target);
    Field("size", Human(std::filesystem::file_size(target, ec)));
    return 0;
}

void PrintPublishHint(std::string_view dump_path, std::string_view suggested_game,
                      std::string_view suggested_label) {
    const zdex::Config config = zdex::LoadConfig();

    std::string command = std::format("zircon publish {}", dump_path);
    if (!suggested_game.empty())  command += std::format(" --game \"{}\"", suggested_game);
    if (!suggested_label.empty()) command += std::format(" --label \"{}\"", suggested_label);

    std::printf("\n%.*sPublish it:%.*s %s\n",
                static_cast<int>(Dim().size()), Dim().data(),
                static_cast<int>(Reset().size()), Reset().data(), command.c_str());
    std::printf("%.*s            browsable at %s within a minute%.*s\n",
                static_cast<int>(Dim().size()), Dim().data(), config.base_url.c_str(),
                static_cast<int>(Reset().size()), Reset().data());
}

} // namespace zircon::app
