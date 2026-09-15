#pragma once

// The publish flow itself: compress, init or resume, send the chunks, finish, wait for the
// import. Everything except how it looks on screen.
//
// It lives here rather than in the CLI because the GUI publishes too, and the one thing this
// project keeps relearning is that the same logic written out twice drifts and only one copy
// gets fixed. The caller supplies hooks and renders the phases however it likes.

#include "zdex/Client.h"
#include "zdex/Gzip.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace zircon::zdex {

struct UploadRequest {
    std::string path;        // the dump on disk: .json, .json.gz or .zip
    std::string game;
    std::string label;
    std::string notes;
    bool        wait{true};  // poll until the import settles
};

struct UploadReport {
    bool         ok{false};
    Outcome      outcome{Outcome::Ok};
    std::string  error;          // the server's code, or "cancelled"
    std::string  message;        // what to put in front of someone

    std::int64_t dump_id{0};
    std::string  url;
    std::string  status;         // queued / indexing / pending / ready / failed
    std::string  status_error;   // set when status is "failed"
    bool         duplicate{false};

    bool         compressed{false};
    GzipStats    stats;

    int          chunk_count{0};
    int          resumed_chunks{-1};   // >= 0 when an earlier session was picked back up
};

enum class UploadPhase { Compressing, Resuming, Uploading, Finishing, Indexing };

struct UploadHooks {
    // done/total are bytes while compressing and uploading, and 0/0 otherwise. `note` carries
    // whatever only that phase knows -- "chunk 3/7", the indexer's own phase label.
    std::function<void(UploadPhase phase, std::uint64_t done, std::uint64_t total,
                       std::string_view note)> progress;

    // Polled between chunks and between status checks. Return false to stop; whatever has
    // already reached the server stays there and the next run resumes it.
    std::function<bool()> keep_going;
};

// Does not look at the config: the caller has already decided there is a key and built the
// client from it. Temporary files it makes, it removes.
UploadReport Upload(Client& client, const UploadRequest& request, const UploadHooks& hooks = {});

} // namespace zircon::zdex
