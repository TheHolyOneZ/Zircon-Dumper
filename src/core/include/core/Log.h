#pragma once

#include <format>
#include <string_view>

namespace zircon::core {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

void SetLogLevel(LogLevel level);

// Mirror everything to a file as well as the console. The injected payload needs this:
// AllocConsole gives it a window that a fullscreen game covers, that some games prevent
// entirely, and that is gone the moment the process exits. A log beside the output is the
// only copy anyone can read afterwards.
//
// Never coloured, whatever the console is doing. Passing an empty path closes it.
void SetLogFile(std::string_view path);
void CloseLogFile();
LogLevel GetLogLevel();
void LogRaw(LogLevel level, std::string_view message);

template <typename... Args>
void Log(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
    if (level < GetLogLevel()) return;
    LogRaw(level, std::format(fmt, std::forward<Args>(args)...));
}

#define ZIRCON_LOG_FN(name, lvl)                                            \
    template <typename... Args>                                             \
    void name(std::format_string<Args...> fmt, Args&&... args) {            \
        Log(LogLevel::lvl, fmt, std::forward<Args>(args)...);               \
    }

ZIRCON_LOG_FN(LogTrace, Trace)
ZIRCON_LOG_FN(LogDebug, Debug)
ZIRCON_LOG_FN(LogInfo,  Info)
ZIRCON_LOG_FN(LogWarn,  Warn)
ZIRCON_LOG_FN(LogError, Error)

#undef ZIRCON_LOG_FN

} // namespace zircon::core
