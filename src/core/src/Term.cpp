#include "core/Term.h"

#include <cstdlib>
#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace zircon::core {

namespace {

bool g_enabled = false;

// Present and non-empty. getenv is deprecated on MSVC and the project builds warning-free.
bool EnvIsSet(const char* name) {
#ifdef _WIN32
    char*  value = nullptr;
    size_t size  = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return false;
    const bool set = value[0] != '\0';
    std::free(value);
    return set;
#else
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0';
#endif
}

bool StdoutIsTerminal() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

#ifdef _WIN32
// Without ENABLE_VIRTUAL_TERMINAL_PROCESSING the escapes print literally, which is worse
// than no colour at all. If the console refuses (old conhost, or a handle that isn't a
// console) colour stays off instead of going out blind.
bool EnableVirtualTerminal(DWORD stream) {
    HANDLE handle = ::GetStdHandle(stream);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return false;

    DWORD mode = 0;
    if (!::GetConsoleMode(handle, &mode)) return false;
    if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) return true;
    return ::SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}
#endif

} // namespace

void InitTerminal() {
    // https://no-color.org - set to anything at all and colour is off. Checked first;
    // someone who sets it has already decided.
    if (EnvIsSet("NO_COLOR")) {
        g_enabled = false;
        return;
    }

    if (!StdoutIsTerminal()) {
        g_enabled = false;
        return;
    }

#ifdef _WIN32
    const bool out = EnableVirtualTerminal(STD_OUTPUT_HANDLE);
    EnableVirtualTerminal(STD_ERROR_HANDLE);   // warnings and errors go here
    g_enabled = out;
#else
    g_enabled = true;
#endif
}

void SetColourEnabled(bool enabled) {
#ifdef _WIN32
    if (enabled) {
        EnableVirtualTerminal(STD_OUTPUT_HANDLE);
        EnableVirtualTerminal(STD_ERROR_HANDLE);
    }
#endif
    g_enabled = enabled;
}

bool Colours() { return g_enabled; }

namespace term {

#define ZIRCON_COLOUR(name, code) \
    std::string_view name() { return g_enabled ? "\x1b[" code "m" : ""; }

ZIRCON_COLOUR(Reset,  "0")
ZIRCON_COLOUR(Bold,   "1")
ZIRCON_COLOUR(Dim,    "2")
ZIRCON_COLOUR(Red,    "91")
ZIRCON_COLOUR(Green,  "92")
ZIRCON_COLOUR(Yellow, "93")
ZIRCON_COLOUR(Blue,   "94")
ZIRCON_COLOUR(Cyan,   "96")
ZIRCON_COLOUR(Grey,   "90")

#undef ZIRCON_COLOUR

} // namespace term
} // namespace zircon::core
