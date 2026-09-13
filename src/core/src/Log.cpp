#include "core/Log.h"

#include "core/Term.h"

#include <cstdio>
#ifdef _WIN32
#include <share.h>
#endif
#include <mutex>
#include <string>

namespace zircon::core {

namespace {
LogLevel  g_level = LogLevel::Info;
std::mutex g_mutex;
std::FILE* g_file = nullptr;

const char* LevelTag(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info ";
        case LogLevel::Warn:  return "warn ";
        case LogLevel::Error: return "error";
    }
    return "?????";
}

// Tag only, never the message. A dumper prints paths, names and hex all day; colouring
// that turns the log into noise and buries the two levels that want attention.
std::string_view LevelColour(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return term::Grey();
        case LogLevel::Debug: return term::Grey();
        case LogLevel::Info:  return term::Blue();
        case LogLevel::Warn:  return term::Yellow();
        case LogLevel::Error: return term::Red();
    }
    return {};
}
} // namespace

void SetLogFile(std::string_view path) {
    std::lock_guard lock(g_mutex);
    if (g_file) { std::fclose(g_file); g_file = nullptr; }
    if (path.empty()) return;

    const std::string owned(path);
#ifdef _WIN32
    // _fsopen, not fopen_s: fopen_s takes the file exclusively, so nothing could read the
    // log while the process that is writing it is still running. Watching a payload work
    // is most of the reason the file exists.
    g_file = _fsopen(owned.c_str(), "w", _SH_DENYWR);
#else
    g_file = std::fopen(owned.c_str(), "w");
#endif
}

void CloseLogFile() { SetLogFile({}); }

void SetLogLevel(LogLevel level) { g_level = level; }
LogLevel GetLogLevel() { return g_level; }

void LogRaw(LogLevel level, std::string_view message) {
    if (level < g_level) return;
    std::lock_guard lock(g_mutex);

    std::FILE* out = level >= LogLevel::Warn ? stderr : stdout;
    const auto colour = LevelColour(level);
    const auto reset  = term::Reset();

    // Scaffolding. Dim the whole line: readable when looked for, ignorable when not.
    const bool dim_body = level <= LogLevel::Debug;

    std::fprintf(out, "%.*s[%s]%.*s %.*s%.*s%.*s\n",
                 static_cast<int>(colour.size()), colour.data(),
                 LevelTag(level),
                 static_cast<int>(reset.size()), reset.data(),
                 dim_body ? static_cast<int>(term::Dim().size()) : 0, term::Dim().data(),
                 static_cast<int>(message.size()), message.data(),
                 dim_body ? static_cast<int>(reset.size()) : 0, reset.data());

    if (g_file) {
        std::fprintf(g_file, "[%s] %.*s\n", LevelTag(level),
                     static_cast<int>(message.size()), message.data());
        // Flushed per line on purpose. The interesting log is the one written by a payload
        // that then crashed the game, and a buffered tail is exactly the part that matters.
        std::fflush(g_file);
    }
}

} // namespace zircon::core
