#include "core/Log.h"
#include "core/MemorySource.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <format>

namespace zircon::core {
namespace {

// A guarded copy inside our own address space. Lives in its own tiny function with no
// unwindable objects, since MSVC won't compile __try/__except in a function that needs
// C++ unwinding.
//
// memcpy won't do. The reflection walker follows pointers out of game structures, and one
// stale pointer takes the game process down with us. VirtualQuery first isn't enough
// either: the page can be freed between the query and the copy.
#pragma warning(push)
#pragma warning(disable : 4509)   // SEH with destructible objects; there are none here
static std::size_t GuardedCopy(void* dest, const void* src, std::size_t size) noexcept {
    __try {
        std::memcpy(dest, src, size);
        return size;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        return 0;
    }
}
#pragma warning(pop)

class InternalMemorySource final : public IMemorySource {
public:
    InternalMemorySource() {
        RefreshModules();
        RefreshRegions();
    }

    std::size_t Read(Address addr, void* out, std::size_t size) override {
        if (IsNull(addr)) return 0;

        const auto* src = reinterpret_cast<const void*>(Raw(addr));
        const std::size_t copied = GuardedCopy(out, src, size);
        if (copied) return copied;

        // Whole copy faulted. Retry page by page so a read that merely straddles into an
        // unmapped page still returns the valid part.
        return ReadPageWise(addr, out, size);
    }

    bool EnableWrites(bool enable) override {
        writes_enabled_ = enable;
        return writes_enabled_;
    }

    bool Write(Address addr, const void* in, std::size_t size) override {
        if (!writes_enabled_ || IsNull(addr)) return false;

        DWORD old_protect = 0;
        auto* dest = reinterpret_cast<void*>(Raw(addr));
        if (!::VirtualProtect(dest, size, PAGE_EXECUTE_READWRITE, &old_protect)) return false;

        const std::size_t copied = GuardedCopy(dest, in, size);

        DWORD restored = 0;
        ::VirtualProtect(dest, size, old_protect, &restored);
        return copied == size;
    }

    std::span<const ModuleInfo> Modules() const override { return modules_; }
    std::span<const RegionInfo> Regions() const override { return regions_; }

    Capabilities Caps() const override {
        return Capabilities{
            /*live_objects*/       true,
            /*writable*/           writes_enabled_,
            /*can_call*/           true,    // we are in the process; game functions are callable
            /*full_address_space*/ true,
        };
    }

    std::string Describe() const override {
        return std::format("in-process (pid {}), {} modules",
                           ::GetCurrentProcessId(), modules_.size());
    }

private:
    std::size_t ReadPageWise(Address addr, void* out, std::size_t size) {
        constexpr std::size_t kPageSize = 4096;
        auto* dest = static_cast<std::uint8_t*>(out);
        std::size_t done = 0;

        while (done < size) {
            const std::uint64_t cur = Raw(addr) + done;
            const std::size_t in_page = static_cast<std::size_t>(cur & (kPageSize - 1));
            const std::size_t want = std::min(size - done, kPageSize - in_page);

            const auto* src = reinterpret_cast<const void*>(cur);
            if (GuardedCopy(dest + done, src, want) != want) break;
            done += want;
        }
        return done;
    }

    void RefreshModules() {
        HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,
                                                     ::GetCurrentProcessId());
        if (snapshot == INVALID_HANDLE_VALUE) {
            LogWarn("cannot enumerate own modules");
            return;
        }

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (::Module32FirstW(snapshot, &entry)) {
            do {
                modules_.push_back(ModuleInfo{
                    Narrow(entry.szModule),
                    Narrow(entry.szExePath),
                    static_cast<Address>(reinterpret_cast<std::uint64_t>(entry.modBaseAddr)),
                    entry.modBaseSize,
                });
            } while (::Module32NextW(snapshot, &entry));
        }
        ::CloseHandle(snapshot);
    }

    void RefreshRegions() {
        MEMORY_BASIC_INFORMATION info{};
        std::uint64_t address = 0;

        while (::VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) ==
               sizeof(info)) {
            if (info.State == MEM_COMMIT && info.Protect != 0 &&
                !(info.Protect & PAGE_GUARD) && !(info.Protect & PAGE_NOACCESS)) {
                auto protect = RegionProtect::None;
                switch (info.Protect & 0xFF) {
                    case PAGE_READONLY: protect = RegionProtect::Read; break;
                    case PAGE_READWRITE:
                    case PAGE_WRITECOPY: protect = RegionProtect::Read | RegionProtect::Write; break;
                    case PAGE_EXECUTE: protect = RegionProtect::Execute; break;
                    case PAGE_EXECUTE_READ:
                        protect = RegionProtect::Read | RegionProtect::Execute; break;
                    case PAGE_EXECUTE_READWRITE:
                    case PAGE_EXECUTE_WRITECOPY:
                        protect = RegionProtect::Read | RegionProtect::Write | RegionProtect::Execute;
                        break;
                    default: break;
                }

                regions_.push_back(RegionInfo{
                    static_cast<Address>(reinterpret_cast<std::uint64_t>(info.BaseAddress)),
                    info.RegionSize, protect, info.Type == MEM_IMAGE,
                });
            }

            const std::uint64_t next =
                reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
            if (next <= address) break;
            address = next;
        }
    }

    static std::string Narrow(const wchar_t* text) {
        if (!text) return {};
        const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                                 nullptr, nullptr);
        if (needed <= 1) return {};

        std::string out(static_cast<std::size_t>(needed - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
        return out;
    }

    bool                    writes_enabled_{false};
    std::vector<ModuleInfo> modules_;
    std::vector<RegionInfo> regions_;
};

} // namespace

Result<std::unique_ptr<IMemorySource>> OpenInternal() {
    return std::unique_ptr<IMemorySource>(std::make_unique<InternalMemorySource>());
}

} // namespace zircon::core
