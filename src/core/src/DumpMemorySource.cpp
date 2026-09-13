#include "core/Log.h"
#include "core/MemorySource.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <format>

namespace zircon::core {
namespace {

// Declared here rather than pulled from dbghelp.h. These dumps come from other tools on
// other Windows versions. The on-disk layout is the contract here; the local SDK headers
// describe whatever the local SDK happens to be.

constexpr std::uint32_t kMiniDumpSignature = 0x504D444D;   // "MDMP"

enum StreamType : std::uint32_t {
    kThreadListStream     = 3,
    kModuleListStream     = 4,
    kMemoryListStream     = 5,
    kSystemInfoStream     = 7,
    kMemory64ListStream   = 9,
    kMemoryInfoListStream = 16,
};

// The dump's record of VirtualQueryEx output. Only present when captured with
// MiniDumpWithFullMemoryInfo; without it there is no page protection in the dump at all.
struct MemoryProtectionRecord {
    std::uint64_t base{};
    std::uint64_t size{};
    std::uint32_t protect{};
    std::uint32_t state{};
    std::uint32_t type{};
};

constexpr std::uint32_t kPageNoAccess          = 0x01;
constexpr std::uint32_t kPageReadOnly          = 0x02;
constexpr std::uint32_t kPageReadWrite         = 0x04;
constexpr std::uint32_t kPageWriteCopy         = 0x08;
constexpr std::uint32_t kPageExecute           = 0x10;
constexpr std::uint32_t kPageExecuteRead       = 0x20;
constexpr std::uint32_t kPageExecuteReadWrite  = 0x40;
constexpr std::uint32_t kPageExecuteWriteCopy  = 0x80;
constexpr std::uint32_t kMemImage              = 0x1000000;

RegionProtect ProtectFromDumpRecord(std::uint32_t protect) {
    switch (protect & 0xFF) {
        case kPageReadOnly:         return RegionProtect::Read;
        case kPageReadWrite:
        case kPageWriteCopy:        return RegionProtect::Read | RegionProtect::Write;
        case kPageExecute:          return RegionProtect::Execute;
        case kPageExecuteRead:      return RegionProtect::Read | RegionProtect::Execute;
        case kPageExecuteReadWrite:
        case kPageExecuteWriteCopy:
            return RegionProtect::Read | RegionProtect::Write | RegionProtect::Execute;
        case kPageNoAccess:
        default:                    return RegionProtect::None;
    }
}

// Mapped, not read into a vector. Full-memory dumps of a UE game run to several
// gigabytes, and paging on demand is the difference between usable and not.
class MappedFile {
public:
    ~MappedFile() {
        if (view_)    ::UnmapViewOfFile(view_);
        if (mapping_) ::CloseHandle(mapping_);
        if (file_ && file_ != INVALID_HANDLE_VALUE) ::CloseHandle(file_);
    }

    MappedFile() = default;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    bool Open(std::string_view path, std::string& error) {
        const std::wstring wide = Widen(path);
        file_ = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            error = std::format("cannot open '{}' (error {})", path, ::GetLastError());
            return false;
        }

        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(file_, &size) || size.QuadPart == 0) {
            error = std::format("'{}' is empty or unreadable", path);
            return false;
        }
        size_ = static_cast<std::size_t>(size.QuadPart);

        mapping_ = ::CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping_) {
            error = std::format("cannot map '{}' (error {})", path, ::GetLastError());
            return false;
        }

        view_ = ::MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0);
        if (!view_) {
            error = std::format("cannot view '{}' (error {})", path, ::GetLastError());
            return false;
        }
        return true;
    }

    const std::uint8_t* Data() const { return static_cast<const std::uint8_t*>(view_); }
    std::size_t Size() const { return size_; }

    template <typename T>
    bool At(std::size_t offset, T& out) const {
        if (offset + sizeof(T) > size_) return false;
        std::memcpy(&out, Data() + offset, sizeof(T));
        return true;
    }

private:
    static std::wstring Widen(std::string_view text) {
        const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                                 static_cast<int>(text.size()), nullptr, 0);
        std::wstring out(static_cast<std::size_t>(needed), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                              out.data(), needed);
        return out;
    }

    HANDLE      file_{INVALID_HANDLE_VALUE};
    HANDLE      mapping_{};
    void*       view_{};
    std::size_t size_{};
};

struct MemoryRange {
    std::uint64_t start{};
    std::uint64_t size{};
    std::uint64_t file_offset{};
};

class DumpMemorySource final : public IMemorySource {
public:
    DumpMemorySource(std::unique_ptr<MappedFile> file, std::string name)
        : file_(std::move(file)), name_(std::move(name)) {}

    std::size_t Read(Address addr, void* out, std::size_t size) override {
        auto* dest = static_cast<std::uint8_t*>(out);
        std::size_t done = 0;

        while (done < size) {
            const std::uint64_t cur = Raw(addr) + done;
            const MemoryRange* range = FindRange(cur);
            if (!range) break;

            const std::uint64_t into = cur - range->start;
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uint64_t>(size - done, range->size - into));

            const std::uint64_t file_offset = range->file_offset + into;
            if (file_offset + want > file_->Size()) break;

            std::memcpy(dest + done, file_->Data() + file_offset, want);
            done += want;
        }
        return done;
    }

    std::span<const ModuleInfo> Modules() const override { return modules_; }
    std::span<const RegionInfo> Regions() const override { return regions_; }

    Capabilities Caps() const override {
        return Capabilities{
            /*live_objects*/       has_full_memory_,
            /*writable*/           false,
            /*can_call*/           false,
            /*full_address_space*/ has_full_memory_,
        };
    }

    std::string Describe() const override {
        return std::format("minidump '{}': {} modules, {} memory ranges, {} MiB captured{}",
                           name_, modules_.size(), ranges_.size(), captured_bytes_ >> 20,
                           has_full_memory_ ? "" : " (NOT a full-memory dump)");
    }

    // --- parsing ------------------------------------------------------------------

    bool Parse(std::string& error) {
        std::uint32_t signature{}, version{}, stream_count{}, directory_rva{};
        if (!file_->At(0, signature) || signature != kMiniDumpSignature) {
            error = "not a minidump (missing MDMP signature)";
            return false;
        }
        file_->At(4, version);
        if (!file_->At(8, stream_count) || !file_->At(12, directory_rva)) {
            error = "malformed minidump (truncated header)";
            return false;
        }

        constexpr std::uint32_t kMaxStreams = 4096;
        if (stream_count > kMaxStreams) {
            error = std::format("malformed minidump (implausible stream count {})", stream_count);
            return false;
        }

        for (std::uint32_t i = 0; i < stream_count; ++i) {
            const std::size_t entry = directory_rva + static_cast<std::size_t>(i) * 12;
            std::uint32_t type{}, data_size{}, rva{};
            if (!file_->At(entry, type) || !file_->At(entry + 4, data_size) ||
                !file_->At(entry + 8, rva))
                break;

            switch (type) {
                case kMemory64ListStream:   ParseMemory64List(rva);   break;
                case kMemoryListStream:     ParseMemoryList(rva);     break;
                case kModuleListStream:     ParseModuleList(rva);     break;
                case kMemoryInfoListStream: ParseMemoryInfoList(rva); break;
                default: break;
            }
        }

        if (ranges_.empty()) {
            error = "minidump contains no readable memory ranges";
            return false;
        }

        std::sort(ranges_.begin(), ranges_.end(),
                  [](const MemoryRange& a, const MemoryRange& b) { return a.start < b.start; });

        std::sort(protection_.begin(), protection_.end(),
                  [](const MemoryProtectionRecord& a, const MemoryProtectionRecord& b) {
                      return a.base < b.base;
                  });

        captured_bytes_ = 0;
        regions_.reserve(ranges_.size());
        for (const auto& range : ranges_) {
            captured_bytes_ += range.size;

            // Prefer the dump's own recorded protection. With no MemoryInfoList we simply
            // don't know, and claiming read-only would be worse than admitting it: a caller
            // filtering for writable memory gets silence, which reads as "no such data"
            // rather than "this dump can't answer that".
            RegionProtect protect = RegionProtect::Read | RegionProtect::Write |
                                    RegionProtect::Execute;
            bool is_image = false;

            if (const auto* record = FindProtection(range.start)) {
                protect  = ProtectFromDumpRecord(record->protect);
                is_image = record->type == kMemImage;
            }

            regions_.push_back(RegionInfo{
                static_cast<Address>(range.start), range.size, protect, is_image,
            });
        }

        if (protection_.empty()) {
            LogDebug("'{}' has no MemoryInfoList stream; page protection is unknown and "
                     "every region is reported as read/write/execute. Capture with "
                     "MiniDumpWithFullMemoryInfo for accurate protection.", name_);
        }

        // A dump holding only thread stacks and module headers can't be walked for
        // objects. Detect by stream type: MiniDumpWithFullMemory emits a Memory64List,
        // triage dumps emit a MemoryList. Size is a poor proxy in both directions — a small
        // process dumped in full is legitimately tiny, and a triage dump of a game with
        // many threads is legitimately not.
        has_full_memory_ = saw_memory64_;

        if (!has_full_memory_) {
            LogWarn("'{}' has no Memory64List stream ({} MiB captured across {} ranges), "
                    "so it is a triage minidump rather than a full-memory dump. Object "
                    "enumeration will be unavailable. Recapture with MiniDumpWithFullMemory.",
                    name_, captured_bytes_ >> 20, ranges_.size());
        }

        LogDebug("minidump version {:#x}: {} ranges, {} modules, {} MiB",
                 version, ranges_.size(), modules_.size(), captured_bytes_ >> 20);
        return true;
    }

private:
    const MemoryRange* FindRange(std::uint64_t address) const {
        // Sorted and non-overlapping, so upper_bound lands just past the candidate. A
        // linear search here would dominate every read.
        const auto it = std::upper_bound(ranges_.begin(), ranges_.end(), address,
            [](std::uint64_t value, const MemoryRange& r) { return value < r.start; });
        if (it == ranges_.begin()) return nullptr;

        const MemoryRange& candidate = *(it - 1);
        if (address >= candidate.start && address < candidate.start + candidate.size)
            return &candidate;
        return nullptr;
    }

    void ParseMemory64List(std::uint32_t rva) {
        std::uint64_t count{}, base_rva{};
        if (!file_->At(rva, count) || !file_->At(rva + 8, base_rva)) return;

        constexpr std::uint64_t kMaxRanges = 1ull << 22;
        if (count > kMaxRanges) {
            LogWarn("minidump declares {} memory ranges; refusing as implausible", count);
            return;
        }

        // Memory64 stores descriptors contiguously, with the actual bytes packed back to
        // back starting at base_rva, so the file offset accumulates as we walk.
        saw_memory64_ = true;
        std::uint64_t offset = base_rva;
        ranges_.reserve(ranges_.size() + static_cast<std::size_t>(count));

        for (std::uint64_t i = 0; i < count; ++i) {
            const std::size_t entry = rva + 16 + static_cast<std::size_t>(i) * 16;
            std::uint64_t start{}, size{};
            if (!file_->At(entry, start) || !file_->At(entry + 8, size)) break;
            if (size == 0) continue;

            ranges_.push_back(MemoryRange{start, size, offset});
            offset += size;
        }
    }

    void ParseMemoryList(std::uint32_t rva) {
        std::uint32_t count{};
        if (!file_->At(rva, count)) return;

        constexpr std::uint32_t kMaxRanges = 1u << 22;
        if (count > kMaxRanges) return;

        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t entry = rva + 4 + static_cast<std::size_t>(i) * 16;
            std::uint64_t start{};
            std::uint32_t size{}, data_rva{};
            if (!file_->At(entry, start) || !file_->At(entry + 8, size) ||
                !file_->At(entry + 12, data_rva))
                break;
            if (size == 0) continue;

            ranges_.push_back(MemoryRange{start, size, data_rva});
        }
    }

    void ParseMemoryInfoList(std::uint32_t rva) {
        std::uint32_t header_size{}, entry_size{};
        std::uint64_t count{};
        if (!file_->At(rva, header_size) || !file_->At(rva + 4, entry_size) ||
            !file_->At(rva + 8, count))
            return;

        // Self-describing sizes, so the format can grow. Hardcode 48 and a newer dump
        // parses misaligned.
        if (entry_size < 48) return;

        constexpr std::uint64_t kMaxRecords = 1ull << 22;
        if (count > kMaxRecords) return;

        protection_.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::size_t entry = rva + header_size + static_cast<std::size_t>(i * entry_size);

            MemoryProtectionRecord record;
            if (!file_->At(entry + 0,  record.base)    ||
                !file_->At(entry + 24, record.size)    ||
                !file_->At(entry + 32, record.state)   ||
                !file_->At(entry + 36, record.protect) ||
                !file_->At(entry + 40, record.type))
                break;

            if (record.size == 0) continue;
            protection_.push_back(record);
        }
    }

    const MemoryProtectionRecord* FindProtection(std::uint64_t address) const {
        const auto it = std::upper_bound(protection_.begin(), protection_.end(), address,
            [](std::uint64_t value, const MemoryProtectionRecord& r) { return value < r.base; });
        if (it == protection_.begin()) return nullptr;

        const auto& candidate = *(it - 1);
        if (address >= candidate.base && address < candidate.base + candidate.size)
            return &candidate;
        return nullptr;
    }

    void ParseModuleList(std::uint32_t rva) {
        std::uint32_t count{};
        if (!file_->At(rva, count)) return;

        constexpr std::uint32_t kMaxModules = 1u << 16;
        if (count > kMaxModules) return;

        modules_.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            // MINIDUMP_MODULE is 108 bytes: base(8) size(4) checksum(4) timestamp(4)
            // nameRva(4) VS_FIXEDFILEINFO(52) cvRecord(8) miscRecord(8) reserved(16).
            const std::size_t entry = rva + 4 + static_cast<std::size_t>(i) * 108;
            std::uint64_t base{};
            std::uint32_t size{}, name_rva{};
            if (!file_->At(entry, base) || !file_->At(entry + 8, size) ||
                !file_->At(entry + 20, name_rva))
                break;

            modules_.push_back(ModuleInfo{
                LeafName(ReadMinidumpString(name_rva)),
                ReadMinidumpString(name_rva),
                static_cast<Address>(base),
                size,
            });
        }
    }

    std::string ReadMinidumpString(std::uint32_t rva) const {
        std::uint32_t byte_length{};
        if (!file_->At(rva, byte_length)) return {};

        constexpr std::uint32_t kMaxPathBytes = 64 * 1024;
        if (byte_length == 0 || byte_length > kMaxPathBytes) return {};
        if (rva + 4 + byte_length > file_->Size()) return {};

        const auto* wide = reinterpret_cast<const wchar_t*>(file_->Data() + rva + 4);
        const int chars = static_cast<int>(byte_length / sizeof(wchar_t));

        const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide, chars,
                                                 nullptr, 0, nullptr, nullptr);
        if (needed <= 0) return {};

        std::string out(static_cast<std::size_t>(needed), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, wide, chars, out.data(), needed, nullptr, nullptr);
        return out;
    }

    static std::string LeafName(const std::string& path) {
        const auto slash = path.find_last_of("\\/");
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    std::unique_ptr<MappedFile> file_;
    std::string                 name_;
    std::vector<MemoryRange>    ranges_;
    std::vector<MemoryProtectionRecord> protection_;
    std::vector<ModuleInfo>     modules_;
    std::vector<RegionInfo>     regions_;
    std::uint64_t               captured_bytes_{};
    bool                        saw_memory64_{false};
    bool                        has_full_memory_{false};
};

} // namespace

Result<std::unique_ptr<IMemorySource>> OpenDumpFile(std::string_view path) {
    auto file = std::make_unique<MappedFile>();
    std::string error;
    if (!file->Open(path, error)) return Error{error, 7};

    const auto slash = path.find_last_of("\\/");
    std::string name{slash == std::string_view::npos ? path : path.substr(slash + 1)};

    auto source = std::make_unique<DumpMemorySource>(std::move(file), std::move(name));
    if (!source->Parse(error)) return Error{error, 7};

    return std::unique_ptr<IMemorySource>(std::move(source));
}

} // namespace zircon::core
