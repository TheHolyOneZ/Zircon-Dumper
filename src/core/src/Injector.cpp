// Nothing clever here: write the DLL path into the target, call LoadLibraryW on it
// through a remote thread. That's the loader's own documented entry point.
//
// Plain on purpose. Manual mapping, thread hijacking, unlinking the module from the loader
// lists — all of it is the "detection bypass" this project's scope rules out, and none of
// it buys anything for the job Zircon actually does.

#include "core/Injector.h"
#include "core/Log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#include <cwctype>

#include <filesystem>
#include <format>
#include <string>
#include <vector>

namespace zircon::core {
namespace {

struct HandleGuard {
    HANDLE h{nullptr};
    ~HandleGuard() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
};

// A guardrail, not an obstacle to route around. Zircon reads memory and every feature
// works externally, so if a game runs anti-cheat, use --pid from outside.
// Lower case, because the comparison is.
const wchar_t* kProtectedModules[] = {
    L"easyanticheat", L"beclient", L"battleye", L"vgk", L"vgc", L"mhyprot",
};

std::string LoadedProtection(HANDLE process) {
    HMODULE modules[1024];
    DWORD needed = 0;
    if (!::EnumProcessModulesEx(process, modules, sizeof(modules), &needed, LIST_MODULES_ALL))
        return {};

    const std::size_t count = needed / sizeof(HMODULE);
    for (std::size_t i = 0; i < count; ++i) {
        wchar_t name[MAX_PATH] = {};
        if (!::GetModuleBaseNameW(process, modules[i], name, MAX_PATH)) continue;

        std::wstring lowered(name);
        for (auto& c : lowered) c = static_cast<wchar_t>(::towlower(c));

        for (const wchar_t* marker : kProtectedModules) {
            if (lowered.find(marker) == std::wstring::npos) continue;
            std::string narrow;
            for (const wchar_t* c = name; *c; ++c)
                narrow.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
            return narrow;
        }
    }
    return {};
}

} // namespace

Result<void> Inject(std::uint32_t pid, std::string_view dll_path) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(dll_path, ec);
    if (ec || !std::filesystem::exists(absolute, ec))
        return Error{std::format("payload not found: {}", std::string(dll_path))};

    const std::wstring wide = absolute.wstring();
    const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);

    HandleGuard process{::OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                      PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                      FALSE, pid)};
    if (!process.h) {
        return Error{std::format("cannot open pid {} for injection ({}). Injection needs the "
                                 "same privilege level as the target — try running Zircon as "
                                 "administrator.", pid, ::GetLastError())};
    }

    if (const std::string protection = LoadedProtection(process.h); !protection.empty()) {
        return Error{std::format(
            "pid {} has {} loaded. Zircon does not inject into protected processes; every "
            "feature except calling game functions works externally, so use --pid instead.",
            pid, protection)};
    }

    // Same architecture, or LoadLibraryW sits elsewhere and the thread jumps into nothing.
    BOOL remote_wow64 = FALSE, self_wow64 = FALSE;
    ::IsWow64Process(process.h, &remote_wow64);
    ::IsWow64Process(::GetCurrentProcess(), &self_wow64);
    if (remote_wow64 != self_wow64)
        return Error{std::format("pid {} is a different architecture than this build of "
                                 "Zircon; use the matching one", pid)};

    void* remote = ::VirtualAllocEx(process.h, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                    PAGE_READWRITE);
    if (!remote)
        return Error{std::format("could not allocate {} bytes in pid {} ({})", bytes, pid,
                                 ::GetLastError())};

    SIZE_T written = 0;
    if (!::WriteProcessMemory(process.h, remote, wide.c_str(), bytes, &written) ||
        written != bytes) {
        ::VirtualFreeEx(process.h, remote, 0, MEM_RELEASE);
        return Error{std::format("could not write the payload path into pid {} ({})", pid,
                                 ::GetLastError())};
    }

    // kernel32 loads at the same address in every process of a session, so our
    // LoadLibraryW is also theirs.
    auto* loader = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    if (!loader) {
        ::VirtualFreeEx(process.h, remote, 0, MEM_RELEASE);
        return Error{"could not resolve LoadLibraryW"};
    }

    HandleGuard thread{::CreateRemoteThread(process.h, nullptr, 0, loader, remote, 0, nullptr)};
    if (!thread.h) {
        ::VirtualFreeEx(process.h, remote, 0, MEM_RELEASE);
        return Error{std::format("could not start the remote thread in pid {} ({})", pid,
                                 ::GetLastError())};
    }

    // LoadLibraryW returns when DllMain does, and our DllMain only spawns the payload
    // thread. So this waits on the load only.
    const DWORD waited = ::WaitForSingleObject(thread.h, 10000);
    DWORD module_handle = 0;
    ::GetExitCodeThread(thread.h, &module_handle);

    ::VirtualFreeEx(process.h, remote, 0, MEM_RELEASE);

    if (waited == WAIT_TIMEOUT)
        return Error{"the remote loader did not return within 10s; the target may be "
                     "suspended or stalled in its loader lock"};

    // The thread exit code is LoadLibraryW's module handle truncated to 32 bits. Zero
    // means it really did fail to load, usually a missing runtime DLL.
    if (module_handle == 0)
        return Error{std::format("the target loaded nothing from {}. The payload's runtime "
                                 "dependencies must be present next to it.",
                                 absolute.string())};

    LogInfo("payload loaded into pid {}", pid);
    return {};
}

} // namespace zircon::core
