#include "zdex/Upload.h"

#include "zdex/Config.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace zircon::zdex {
namespace {

// Below this, compressing costs more than it saves and can actively mislead: a 971-byte file
// gzips to 92, which trips the server's 200-byte floor and reports "too small" about a file
// that is nothing of the sort. Send small files as they are and let the server say what is
// actually wrong with them.
constexpr std::uint64_t kWorthCompressing = 64u * 1024u;

constexpr int kPollSeconds = 2;
constexpr int kPollMinutes = 10;

bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string ReadWholeFile(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "cannot open " + path; return {}; }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

UploadReport Failed(const Result& result) {
    UploadReport report;
    report.outcome = result.outcome;
    report.error   = result.error;
    report.message = result.message;
    return report;
}

UploadReport Refused(Outcome outcome, std::string error, std::string message) {
    UploadReport report;
    report.outcome = outcome;
    report.error   = std::move(error);
    report.message = std::move(message);
    return report;
}

} // namespace

UploadReport Upload(Client& client, const UploadRequest& request, const UploadHooks& hooks) {
    const auto tell = [&](UploadPhase phase, std::uint64_t done, std::uint64_t total,
                          std::string_view note) {
        if (hooks.progress) hooks.progress(phase, done, total, note);
    };
    const auto carry_on = [&] { return !hooks.keep_going || hooks.keep_going(); };

    std::error_code ec;
    if (!std::filesystem::exists(request.path, ec))
        return Refused(Outcome::Usage, "no_file", "no such file: " + request.path);
    if (request.game.empty() || request.label.empty())
        return Refused(Outcome::Usage, "no_name", "publishing needs a game and a label");

    const bool already_packed = EndsWith(request.path, ".gz") || EndsWith(request.path, ".zip");
    if (!already_packed && !EndsWith(request.path, ".json"))
        return Refused(Outcome::Usage, "not_a_dump",
                       "publish takes a .json, .json.gz or .zip dump; the server rejects "
                       "SDK headers and usmap files");

    UploadReport report;

    // --- gzip, unless it already is --------------------------------------------------
    const auto raw_bytes = std::filesystem::file_size(request.path, ec);
    std::string send_path = request.path;
    std::string temp_path;

    if (!already_packed && raw_bytes >= kWorthCompressing) {
        temp_path = (std::filesystem::temp_directory_path() /
                     (std::filesystem::path(request.path).stem().string() + ".json.gz")).string();

        std::string error;
        const bool ok = GzipFile(request.path, temp_path, error, &report.stats,
            [&](std::uint64_t seen) {
                tell(UploadPhase::Compressing, seen, raw_bytes, {});
                return carry_on();
            });

        if (!ok) {
            std::filesystem::remove(temp_path, ec);
            if (!carry_on()) return Refused(Outcome::Usage, "cancelled", "cancelled");
            return Refused(Outcome::Usage, "compress", error);
        }
        report.compressed = true;
        send_path = temp_path;
    }

    const auto cleanup = [&] {
        if (!temp_path.empty()) std::filesystem::remove(temp_path, ec);
    };

    std::string read_error;
    const std::string payload = ReadWholeFile(send_path, read_error);
    if (payload.empty()) {
        cleanup();
        return Refused(Outcome::Usage, "empty",
                       read_error.empty() ? "the file is empty" : read_error);
    }

    const std::string filename = std::filesystem::path(send_path).filename().string();

    // --- init, or resume -------------------------------------------------------------
    // Fingerprint the dump that was named, not what goes on the wire. Compressing writes a
    // new temp file every run, so its timestamp is new every run and the stored fingerprint
    // never matches -- resume had literally never fired. The source is the better key
    // anyway: the encoder writes mtime 0, so the same dump always gzips to the same bytes.
    const std::string fingerprint = FingerprintFile(request.path);
    InitResult session;
    bool resumed = false;

    for (const auto& record : LoadUploads()) {
        if (record.fingerprint != fingerprint || record.upload_id.empty()) continue;

        // Belt and braces. The fingerprint covers the source already, but resuming means
        // sending chunk N of something the server half has. If the payload is not the length
        // it expects -- a later build compressing differently, say -- start over instead of
        // splicing two of them together.
        if (record.size != payload.size()) {
            ForgetUpload(fingerprint);
            break;
        }

        const Result status = client.UploadStatus(record.upload_id, session);
        if (status.ok && session.chunk_bytes > 0) {
            resumed = true;
            report.resumed_chunks = static_cast<int>(session.received.size());
            tell(UploadPhase::Resuming, 0, 0, {});
        } else {
            ForgetUpload(fingerprint);   // 410 closed, or the server forgot it
        }
        break;
    }

    if (!resumed) {
        const Result init = client.Init(filename, payload.size(), request.game, request.label,
                                        request.notes, session);
        if (!init.ok) { cleanup(); return Failed(init); }

        UploadRecord record;
        record.fingerprint = fingerprint;
        record.upload_id   = session.upload_id;
        record.size        = payload.size();
        record.game        = request.game;
        record.label       = request.label;
        RememberUpload(record);
    }
    report.chunk_count = session.chunk_count;

    // --- chunks ----------------------------------------------------------------------
    const auto send_missing = [&](const InitResult& state, std::string_view verb) -> Result {
        for (int index = 0; index < state.chunk_count; ++index) {
            if (std::find(state.received.begin(), state.received.end(), index) !=
                state.received.end())
                continue;
            if (!carry_on()) {
                Result stop;
                stop.outcome = Outcome::Usage;
                stop.error = stop.message = "cancelled";
                return stop;
            }

            const std::size_t at   = static_cast<std::size_t>(index) * state.chunk_bytes;
            const std::size_t take = std::min<std::size_t>(state.chunk_bytes,
                                                           payload.size() - at);

            const std::string note = std::string(verb) + " chunk " + std::to_string(index + 1) +
                                     "/" + std::to_string(state.chunk_count);
            client.on_progress = [&](std::uint64_t sent, std::uint64_t) {
                tell(UploadPhase::Uploading, at + sent, payload.size(), note);
                return carry_on();
            };

            const Result sent = client.SendChunk(session.upload_id, index,
                                                 std::string_view(payload).substr(at, take));
            client.on_progress = nullptr;
            if (!sent.ok) return sent;
        }
        return Result::Success();
    };

    if (const Result sent = send_missing(session, "sending"); !sent.ok) {
        cleanup();
        return Failed(sent);
    }

    // --- finish ----------------------------------------------------------------------
    tell(UploadPhase::Finishing, 0, 0, {});

    FinishResult finished;
    Result finish = client.Finish(session.upload_id, finished);

    // The server answers 409 with the indices it actually holds; resend those and ask again.
    if (!finish.ok && finish.error == "incomplete") {
        InitResult again;
        if (client.UploadStatus(session.upload_id, again).ok) {
            if (const Result resent = send_missing(again, "re-sending"); !resent.ok) {
                cleanup();
                return Failed(resent);
            }
        }
        finish = client.Finish(session.upload_id, finished);
    }

    cleanup();
    ForgetUpload(fingerprint);

    if (!finish.ok) return Failed(finish);

    report.dump_id   = finished.dump_id;
    report.url       = finished.url;
    report.duplicate = finished.duplicate;
    report.status    = finished.status;
    if (finished.duplicate) report.message = finish.message;

    // --- wait for the import ---------------------------------------------------------
    if (request.wait && !finished.duplicate && finished.dump_id > 0) {
        DumpStatus state;
        state.status = finished.status;

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::minutes(kPollMinutes);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!carry_on()) break;             // the dump exists; the URL is already good
            const Result polled = client.Status(finished.dump_id, state);
            if (!polled.ok) break;
            tell(UploadPhase::Indexing, static_cast<std::uint64_t>(state.progress), 100,
                 state.phase_label);
            if (state.settled()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollSeconds * 1000 + 500));
        }

        report.status       = state.status;
        report.status_error = state.error;
    }

    report.ok = true;
    return report;
}

} // namespace zircon::zdex
