#pragma once

// What the walk was touching when the process went down.
//
// A log line every thousandth type is a thousand-wide window, and no use when you want the one
// class to skip. So the current one goes in a memory-mapped page: a memcpy, no syscall, cheap
// enough to leave on for every dump. Windows writes the dirty page back even when the process
// is torn down, so the next run can name the type.
//
// Removed when a walk finishes. One left on disk means the last one didn't.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace zircon::core {

// Fixed size, so the page is written once and never grows. Longest inflated generic name in
// the corpus is nowhere near this.
constexpr std::size_t kBreadcrumbSize = 1024;

class CrashBreadcrumb {
public:
    CrashBreadcrumb() = default;
    ~CrashBreadcrumb();

    CrashBreadcrumb(const CrashBreadcrumb&)            = delete;
    CrashBreadcrumb& operator=(const CrashBreadcrumb&) = delete;

    // Nothing here is fatal. A breadcrumb we couldn't open just means a crash is reported
    // the way it was before, so the caller doesn't have to care whether this worked.
    bool Open(const std::filesystem::path& path);

    void Note(std::string_view text);

    // Adds a line after whatever Note last wrote. Nothing here allocates or takes a lock,
    // so it is safe to call from an exception handler, where the heap lock may be held by
    // the thread that just faulted.
    void Append(std::string_view line);

    // The walk got through. Closes the mapping and deletes the file.
    void Finish();

    bool IsOpen() const { return view_ != nullptr; }

private:
    void Close();

    void* file_{nullptr};
    void* mapping_{nullptr};
    char* view_{nullptr};
    std::filesystem::path path_;
};

// What a previous run left behind, or nothing. Doesn't delete it -- the caller decides.
std::optional<std::string> ReadBreadcrumb(const std::filesystem::path& path);

// A breadcrumb is a key and, if the writer knew more, what it was doing to it, newline
// separated. The key is the half that identifies the thing and the half a skip list takes.
std::string_view BreadcrumbKey(std::string_view text);
std::string_view BreadcrumbPhase(std::string_view text);

} // namespace zircon::core
