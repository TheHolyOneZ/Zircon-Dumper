#pragma once

#include <format>
#include <string_view>

namespace zircon::core {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

void SetLogLevel(LogLevel level);
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
