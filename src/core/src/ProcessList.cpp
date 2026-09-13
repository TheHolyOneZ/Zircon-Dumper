#include "core/ProcessList.h"
#include "core/Log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

namespace zircon::core {
namespace {

std::string Narrow(const wchar_t* text) {
    if (!text || !*text) return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};

    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

// Best effort. The limited-information right succeeds for far more processes than
// PROCESS_QUERY_INFORMATION and still comes back empty for protected or elevated targets,
// which the caller is happy to treat as unknown.
std::string QueryImagePath(std::uint32_t pid, bool& is_64bit) {
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};

    wchar_t buffer[MAX_PATH * 2] = {};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    std::string path;
    if (::QueryFullProcessImageNameW(process, 0, buffer, &size))
        path = Narrow(buffer);

    BOOL wow64 = FALSE;
    if (::IsWow64Process(process, &wow64)) is_64bit = !wow64;

    ::CloseHandle(process);
    return path;
}

} // namespace

std::vector<ProcessInfo> EnumerateProcesses() {
    std::vector<ProcessInfo> processes;

    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        LogWarn("cannot snapshot running processes (error {})", ::GetLastError());
        return processes;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (::Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == 0) continue;   // idle process

            ProcessInfo info;
            info.pid  = entry.th32ProcessID;
            info.name = Narrow(entry.szExeFile);
            info.path = QueryImagePath(info.pid, info.is_64bit);
            processes.push_back(std::move(info));
        } while (::Process32NextW(snapshot, &entry));
    }

    ::CloseHandle(snapshot);
    return processes;
}

} // namespace zircon::core
