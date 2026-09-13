// The standalone browser executable. Turns a command line into a pid to attach to, and
// that is all: window, device and UI are the same ones the injected payload uses, over in
// Host.cpp and Browser.cpp.

#include "Host.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <cstdint>

#pragma comment(lib, "shell32.lib")

namespace {

// "--attach <pid>" skips the picker. Unrecognised arguments are ignored, not fatal;
// a window that will not open because of a stray argument is worse than one showing the
// picker.
std::uint32_t ParseAttachPid(PWSTR command_line) {
    int count = 0;
    LPWSTR* argv = ::CommandLineToArgvW(command_line, &count);
    if (!argv) return 0;

    std::uint32_t pid = 0;
    for (int i = 0; i + 1 < count; ++i) {
        if (::lstrcmpiW(argv[i], L"--attach") != 0 && ::lstrcmpiW(argv[i], L"-p") != 0) continue;
        pid = static_cast<std::uint32_t>(::wcstoul(argv[i + 1], nullptr, 10));
        break;
    }
    ::LocalFree(argv);
    return pid;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR command_line, int) {
    return zircon::gui::RunBrowserWindow(ParseAttachPid(command_line));
}
