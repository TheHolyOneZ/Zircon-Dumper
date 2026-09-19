#include "core/Breadcrumb.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>
#include <fstream>
#include <system_error>

namespace zircon::core {

CrashBreadcrumb::~CrashBreadcrumb() {
    Close();
}

bool CrashBreadcrumb::Open(const std::filesystem::path& path) {
    Close();

    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);

    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    HANDLE mapping = ::CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0,
                                          static_cast<DWORD>(kBreadcrumbSize), nullptr);
    if (!mapping) {
        ::CloseHandle(file);
        return false;
    }

    void* view = ::MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, kBreadcrumbSize);
    if (!view) {
        ::CloseHandle(mapping);
        ::CloseHandle(file);
        return false;
    }

    file_    = file;
    mapping_ = mapping;
    view_    = static_cast<char*>(view);
    path_    = path;

    std::memset(view_, 0, kBreadcrumbSize);
    return true;
}

void CrashBreadcrumb::Note(std::string_view text) {
    if (!view_) return;

    // One byte spare for the terminator. Anything longer than the page is already more name
    // than you need to find the type.
    const std::size_t length = text.size() < kBreadcrumbSize - 1 ? text.size()
                                                                 : kBreadcrumbSize - 1;
    std::memcpy(view_, text.data(), length);
    view_[length] = '\0';
}

void CrashBreadcrumb::Append(std::string_view line) {
    if (!view_) return;

    // Where the current text ends. Scanned rather than tracked, because the thread that
    // wrote it is not necessarily the thread appending.
    std::size_t at = 0;
    while (at < kBreadcrumbSize && view_[at] != '\0') ++at;
    if (at + 2 >= kBreadcrumbSize) return;

    view_[at++] = '\n';
    const std::size_t room = kBreadcrumbSize - at - 1;
    const std::size_t length = line.size() < room ? line.size() : room;
    std::memcpy(view_ + at, line.data(), length);
    view_[at + length] = '\0';
}

void CrashBreadcrumb::Finish() {
    if (!view_) return;
    const auto path = path_;
    Close();

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void CrashBreadcrumb::Close() {
    if (view_)    { ::UnmapViewOfFile(view_); view_ = nullptr; }
    if (mapping_) { ::CloseHandle(static_cast<HANDLE>(mapping_)); mapping_ = nullptr; }
    if (file_)    { ::CloseHandle(static_cast<HANDLE>(file_)); file_ = nullptr; }
    path_.clear();
}

std::optional<std::string> ReadBreadcrumb(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;

    std::string buffer(kBreadcrumbSize, '\0');
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    buffer.resize(static_cast<std::size_t>(in.gcount()));

    const auto end = buffer.find('\0');
    if (end != std::string::npos) buffer.resize(end);
    if (buffer.empty()) return std::nullopt;
    return buffer;
}

std::string_view BreadcrumbKey(std::string_view text) {
    const auto split = text.find('\n');
    return split == std::string_view::npos ? text : text.substr(0, split);
}

std::string_view BreadcrumbPhase(std::string_view text) {
    const auto split = text.find('\n');
    return split == std::string_view::npos ? std::string_view{} : text.substr(split + 1);
}

} // namespace zircon::core
