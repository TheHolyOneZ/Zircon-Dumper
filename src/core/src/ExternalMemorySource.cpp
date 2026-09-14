#include "core/Log.h"
#include "core/MemorySource.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <format>

namespace zircon::core {
namespace {

struct HandleCloser {
    void operator()(HANDLE h) const noexcept {
        if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
    }
};
using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser>;

std::string LastErrorText(DWORD code) {
    char* buffer = nullptr;
    const DWORD len = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<char*>(&buffer), 0, nullptr);

    std::string text = len && buffer ? std::string(buffer, len) : std::format("error {}", code);
    if (buffer) ::LocalFree(buffer);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

RegionProtect ProtectFromWin32(DWORD protect) {
    const DWORD access = protect & 0xFF;
    auto out = RegionProtect::None;

    switch (access) {
        case PAGE_READONLY:          out = RegionProtect::Read; break;
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:         out = RegionProtect::Read | RegionProtect::Write; break;
        case PAGE_EXECUTE:           out = RegionProtect::Execute; break;
        case PAGE_EXECUTE_READ:      out = RegionProtect::Read | RegionProtect::Execute; break;
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            out = RegionProtect::Read | RegionProtect::Write | RegionProtect::Execute; break;
        default: break;
    }
    return out;
}

class ExternalMemorySource final : public IMemorySource {
public:
    ExternalMemorySource(UniqueHandle process, DWORD pid, std::string name)
        : process_(std::move(process)), pid_(pid), name_(std::move(name)) {
        RefreshModules();
        RefreshRegions();
    }

    std::size_t Read(Address addr, void* out, std::size_t size) override {
        SIZE_T read = 0;
        if (::ReadProcessMemory(process_.get(), reinterpret_cast<LPCVOID>(Raw(addr)),
                                out, size, &read)) {
            return read;
        }

        // A failed RPM still reports a partial count when it stopped at a page boundary,
        // the common case near the end of a mapped region. Keep the bytes we did get.
        return read;
    }

    bool EnableWrites(bool enable) override {
        if (!enable) {
            writes_enabled_ = false;
            return false;
        }
        if (writes_enabled_) return true;

        // The handle this was opened with can only read. Writing needs a second one with
        // VM_WRITE and VM_OPERATION, so it is asked for here and only here: a session
        // that never enables writes never holds a handle that could perform one.
        constexpr DWORD kWriteAccess = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                                       PROCESS_VM_WRITE | PROCESS_VM_OPERATION;

        UniqueHandle writable(::OpenProcess(kWriteAccess, FALSE, pid_));
        if (!writable) {
            LogWarn("cannot reopen pid {} for writing: {}", pid_,
                    LastErrorText(::GetLastError()));
            return false;
        }

        process_ = std::move(writable);
        writes_enabled_ = true;
        LogInfo("writes enabled on pid {}", pid_);
        return true;
    }

    bool Write(Address addr, const void* in, std::size_t size) override {
        if (!writes_enabled_) return false;

        auto* target = reinterpret_cast<LPVOID>(Raw(addr));
        SIZE_T written = 0;
        if (::WriteProcessMemory(process_.get(), target, in, size, &written) &&
            written == size)
            return true;

        // A read-only page is the usual reason. Lift the protection for the write and put
        // it back, so the target is left exactly as it was found.
        DWORD previous = 0;
        if (!::VirtualProtectEx(process_.get(), target, size, PAGE_EXECUTE_READWRITE,
                                &previous))
            return false;

        written = 0;
        const bool ok = ::WriteProcessMemory(process_.get(), target, in, size, &written) &&
                        written == size;

        DWORD ignored = 0;
        ::VirtualProtectEx(process_.get(), target, size, previous, &ignored);
        return ok;
    }

    std::span<const ModuleInfo> Modules() const override { return modules_; }
    std::span<const RegionInfo> Regions() const override { return regions_; }

    Capabilities Caps() const override {
        return Capabilities{
            /*live_objects*/       true,
            /*writable*/           writes_enabled_,
            /*can_call*/           false,   // that requires being inside the process
            /*full_address_space*/ true,
        };
    }

    std::string Describe() const override {
        return std::format("external process '{}' (pid {}), {} modules",
                           name_, pid_, modules_.size());
    }

private:
    void RefreshModules() {
        // Module32First/Next fails transiently while the target loads DLLs. Normal for a
        // game still starting up, so retry briefly.
        for (int attempt = 0; attempt < 5; ++attempt) {
            UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid_));
            if (snapshot.get() == INVALID_HANDLE_VALUE) {
                ::Sleep(20);
                continue;
            }

            MODULEENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            if (!::Module32FirstW(snapshot.get(), &entry)) {
                ::Sleep(20);
                continue;
            }

            modules_.clear();
            do {
                modules_.push_back(ModuleInfo{
                    Narrow(entry.szModule),
                    Narrow(entry.szExePath),
                    static_cast<Address>(reinterpret_cast<std::uint64_t>(entry.modBaseAddr)),
                    entry.modBaseSize,
                });
            } while (::Module32NextW(snapshot.get(), &entry));
            return;
        }
        LogWarn("could not enumerate modules for pid {}", pid_);
    }

    void RefreshRegions() {
        MEMORY_BASIC_INFORMATION info{};
        std::uint64_t address = 0;

        while (::VirtualQueryEx(process_.get(), reinterpret_cast<LPCVOID>(address),
                                &info, sizeof(info)) == sizeof(info)) {
            if (info.State == MEM_COMMIT && info.Protect != 0 &&
                !(info.Protect & PAGE_GUARD) && !(info.Protect & PAGE_NOACCESS)) {
                regions_.push_back(RegionInfo{
                    static_cast<Address>(reinterpret_cast<std::uint64_t>(info.BaseAddress)),
                    info.RegionSize,
                    ProtectFromWin32(info.Protect),
                    info.Type == MEM_IMAGE,
                });
            }

            const std::uint64_t next =
                reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
            if (next <= address) break;   // guards against a malformed/zero region size
            address = next;
        }
    }

    static std::string Narrow(const wchar_t* text) {
        if (!text) return {};
        const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 1) return {};

        std::string out(static_cast<std::size_t>(needed - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
        return out;
    }

    UniqueHandle            process_;
    DWORD                   pid_{};
    std::string             name_;
    bool                    writes_enabled_{false};
    std::vector<ModuleInfo> modules_;
    std::vector<RegionInfo> regions_;
};

Result<std::unique_ptr<IMemorySource>> AttachTo(DWORD pid, std::string name) {
    // No PROCESS_VM_WRITE. Reading is the whole job.
    constexpr DWORD kAccess = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ;

    UniqueHandle process(::OpenProcess(kAccess, FALSE, pid));
    if (!process) {
        const DWORD code = ::GetLastError();
        std::string hint;
        if (code == ERROR_ACCESS_DENIED)
            hint = " (try running as administrator; the target may also be elevated)";
        return Error{std::format("cannot open pid {}: {}{}", pid, LastErrorText(code), hint), 5};
    }

    if (name.empty()) {
        wchar_t path[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        if (::QueryFullProcessImageNameW(process.get(), 0, path, &size)) {
            const std::wstring full(path, size);
            const auto slash = full.find_last_of(L"\\/");
            const std::wstring leaf = slash == std::wstring::npos ? full : full.substr(slash + 1);
            const int needed = ::WideCharToMultiByte(CP_UTF8, 0, leaf.c_str(), -1,
                                                     nullptr, 0, nullptr, nullptr);
            if (needed > 1) {
                name.resize(static_cast<std::size_t>(needed - 1));
                ::WideCharToMultiByte(CP_UTF8, 0, leaf.c_str(), -1, name.data(), needed,
                                      nullptr, nullptr);
            }
        }
    }

    BOOL wow64 = FALSE;
    if (::IsWow64Process(process.get(), &wow64) && wow64)
        LogWarn("pid {} is a 32-bit (WOW64) process; UE shipping games are normally x64", pid);

    return std::unique_ptr<IMemorySource>(
        std::make_unique<ExternalMemorySource>(std::move(process), pid, std::move(name)));
}

} // namespace

Result<std::unique_ptr<IMemorySource>> OpenExternalByPid(std::uint32_t pid) {
    return AttachTo(static_cast<DWORD>(pid), {});
}

Result<std::unique_ptr<IMemorySource>> OpenExternalByName(std::string_view process_name) {
    UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.get() == INVALID_HANDLE_VALUE)
        return Error{"cannot snapshot running processes: " + LastErrorText(::GetLastError()), 5};

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!::Process32FirstW(snapshot.get(), &entry))
        return Error{"cannot enumerate running processes", 5};

    const auto lower = [](std::string text) {
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    const std::string needle = lower(std::string(process_name));

    // Exact first, substring second.
    //
    // `zircon detect` prints "StormEscape" in its PROJECT column, and the obvious next move
    // is to paste that into --process. That used to fail - the match was the whole file name
    // and nothing else, while `inject` had always taken a substring. Two commands accepting a
    // name the rest of the tool refused.
    //
    // Exact still wins, so a process whose name is a substring of another's can't get
    // shadowed, and an ambiguous substring is refused rather than guessed at.
    std::vector<std::pair<DWORD, std::string>> exact;
    std::vector<std::pair<DWORD, std::string>> partial;

    do {
        const int needed = ::WideCharToMultiByte(CP_UTF8, 0, entry.szExeFile, -1,
                                                 nullptr, 0, nullptr, nullptr);
        if (needed <= 1) continue;

        std::string name(static_cast<std::size_t>(needed - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, entry.szExeFile, -1, name.data(), needed,
                              nullptr, nullptr);

        const std::string folded = lower(name);
        if (folded == needle) exact.emplace_back(entry.th32ProcessID, name);
        else if (folded.find(needle) != std::string::npos)
            partial.emplace_back(entry.th32ProcessID, name);
    } while (::Process32NextW(snapshot.get(), &entry));

    auto& matches = exact.empty() ? partial : exact;

    if (matches.empty())
        return Error{std::format("no running process named '{}'", process_name), 5};

    if (matches.size() > 1) {
        // Attaching to the wrong instance silently dumps the wrong build. Make them pick.
        std::string pids;
        for (const auto& [pid, name] : matches) pids += std::format("{} ({}) ", pid, name);
        return Error{std::format("{} processes match '{}': {}— pass --pid",
                                 matches.size(), process_name, pids), 5};
    }

    return AttachTo(matches.front().first, matches.front().second);
}

} // namespace zircon::core
