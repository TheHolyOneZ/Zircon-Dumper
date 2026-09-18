#pragma once

// The Zdex commands, kept out of main.cpp because between them they are longer than most
// of the other commands put together and none of the rest of the CLI needs to see them.

#include <cstdint>
#include <string>
#include <string_view>

namespace zircon::app {

struct PublishOptions {
    std::string path;        // the dump to send: .json, .json.gz or .zip
    std::string game;
    std::string label;
    std::string notes;
    bool wait{true};         // poll until the import settles
    bool json_output{false};
    bool assume_yes{false};  // skip the one-time terms prompt
    bool open_browser{false};
};

// `login` only ever stores an API key. There is no account, no password and no browser
// round-trip: Zdex issues the key on its own site and this puts it where the CLI can
// find it. `key` empty means prompt for it with the echo off.
int CommandLogin(std::string_view key);
int CommandLogout();

int CommandPublish(const PublishOptions& options);

// Why Zdex won't take this dump, or empty when it will.
//
// Both runtimes publish now. What's left is the guard for a runtime neither side knows,
// refused here rather than sent to a server that would reject it (or half-accept it).
// Tested on what the dump says, never the filename.
std::string PublishRefusal(std::string_view runtime);

// Same question against a file. Reads only the header.
std::string PublishRefusalForFile(std::string_view path);

// Downloads one artefact of a published dump. `kind` is usmap | sdk | json.
int CommandFetch(std::int64_t dump_id, std::string_view kind, std::string_view out_path);

// Printed after a successful `zircon dump` so the next step is in front of the user at
// the moment they have something to publish.
void PrintPublishHint(std::string_view dump_path, std::string_view suggested_game,
                      std::string_view suggested_label);

// Defaults the handoff asks for: the process name without the UE suffix, and the dump's
// own date.
std::string GameNameFromProcess(std::string_view process);
std::string LabelFromTimestamp(std::string_view created_utc);

} // namespace zircon::app
