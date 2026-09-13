#include "core/PeImage.h"
#include "core/Log.h"
#include "core/MemorySource.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace zircon::core {
namespace {

// Every field access in this file goes through these, so a truncated or hostile PE gets a
// clean error instead of a read past the end of the buffer.
template <typename T>
bool At(const std::vector<std::uint8_t>& data, std::size_t offset, T& out) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (offset + sizeof(T) > data.size()) return false;
    std::memcpy(&out, data.data() + offset, sizeof(T));
    return true;
}

constexpr std::uint16_t kDosMagic   = 0x5A4D;      // "MZ"
constexpr std::uint32_t kPeMagic    = 0x00004550;  // "PE\0\0"
constexpr std::uint16_t kOpt32Magic = 0x010B;
constexpr std::uint16_t kOpt64Magic = 0x020B;

constexpr std::uint32_t kSecMemExecute = 0x20000000;
constexpr std::uint32_t kSecMemRead    = 0x40000000;
constexpr std::uint32_t kSecMemWrite   = 0x80000000;

RegionProtect ProtectFromCharacteristics(std::uint32_t flags) {
    auto protect = RegionProtect::None;
    if (flags & kSecMemRead)    protect = protect | RegionProtect::Read;
    if (flags & kSecMemWrite)   protect = protect | RegionProtect::Write;
    if (flags & kSecMemExecute) protect = protect | RegionProtect::Execute;
    return protect;
}

} // namespace

Result<PeImage> PeImage::FromFile(std::string_view path) {
    std::error_code ec;
    const auto fs_path = std::filesystem::path(path);
    const auto size = std::filesystem::file_size(fs_path, ec);
    if (ec) return Error{std::string("cannot stat '") + std::string(path) + "': " + ec.message(), 3};

    std::ifstream file(fs_path, std::ios::binary);
    if (!file) return Error{std::string("cannot open '") + std::string(path) + "'", 3};

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) return Error{std::string("short read on '") + std::string(path) + "'", 3};

    return FromBuffer(std::move(bytes), fs_path.filename().string());
}

Result<PeImage> PeImage::FromBuffer(std::vector<std::uint8_t> bytes, std::string name) {
    PeImage image;
    image.bytes_ = std::move(bytes);
    image.name_  = std::move(name);

    auto parsed = image.Parse();
    if (!parsed) return parsed.error();
    return image;
}

Result<bool> PeImage::Parse() {
    std::uint16_t dos_magic{};
    if (!At(bytes_, 0, dos_magic) || dos_magic != kDosMagic)
        return Error{"not a PE file (missing MZ header)", 4};

    std::int32_t pe_offset{};
    if (!At(bytes_, 0x3C, pe_offset) || pe_offset < 0)
        return Error{"malformed PE (bad e_lfanew)", 4};

    const auto pe = static_cast<std::size_t>(pe_offset);
    std::uint32_t pe_magic{};
    if (!At(bytes_, pe, pe_magic) || pe_magic != kPeMagic)
        return Error{"malformed PE (missing PE signature)", 4};

    // Right after the 4-byte signature.
    std::uint16_t section_count{};
    std::uint16_t optional_size{};
    if (!At(bytes_, pe + 6,  section_count) ||
        !At(bytes_, pe + 20, optional_size))
        return Error{"malformed PE (truncated COFF header)", 4};

    const std::size_t opt = pe + 24;
    std::uint16_t opt_magic{};
    if (!At(bytes_, opt, opt_magic))
        return Error{"malformed PE (truncated optional header)", 4};

    if (opt_magic == kOpt64Magic) {
        is_64bit_ = true;
    } else if (opt_magic == kOpt32Magic) {
        is_64bit_ = false;
    } else {
        return Error{"malformed PE (unknown optional header magic)", 4};
    }

    if (!At(bytes_, opt + 16, entry_point_rva_))
        return Error{"malformed PE (truncated optional header)", 4};

    // ImageBase is 8 bytes at +24 for PE32+, 4 bytes at +28 for PE32.
    if (is_64bit_) {
        if (!At(bytes_, opt + 24, preferred_base_))
            return Error{"malformed PE (truncated ImageBase)", 4};
    } else {
        std::uint32_t base32{};
        if (!At(bytes_, opt + 28, base32))
            return Error{"malformed PE (truncated ImageBase)", 4};
        preferred_base_ = base32;
    }

    if (!At(bytes_, opt + 56, size_of_image_))
        return Error{"malformed PE (truncated SizeOfImage)", 4};
    if (!At(bytes_, opt + 60, size_of_headers_))
        return Error{"malformed PE (truncated SizeOfHeaders)", 4};

    // Data directories start at +96 (PE32) or +112 (PE32+). Export table is index 0.
    const std::size_t dir_base = opt + (is_64bit_ ? 112 : 96);
    std::uint32_t export_rva{}, export_size{};
    const bool have_exports = At(bytes_, dir_base, export_rva) &&
                              At(bytes_, dir_base + 4, export_size);

    const std::size_t section_table = opt + optional_size;
    sections_.reserve(section_count);
    for (std::uint16_t i = 0; i < section_count; ++i) {
        const std::size_t entry = section_table + static_cast<std::size_t>(i) * 40;
        if (entry + 40 > bytes_.size())
            return Error{"malformed PE (truncated section table)", 4};

        PeSection section;
        char raw_name[9] = {};
        std::memcpy(raw_name, bytes_.data() + entry, 8);
        section.name = raw_name;

        std::uint32_t characteristics{};
        At(bytes_, entry +  8, section.virtual_size);
        At(bytes_, entry + 12, section.rva);
        At(bytes_, entry + 16, section.raw_size);
        At(bytes_, entry + 20, section.raw_offset);
        At(bytes_, entry + 36, characteristics);
        section.protect = ProtectFromCharacteristics(characteristics);

        sections_.push_back(std::move(section));
    }

    if (have_exports && export_rva != 0 && export_size != 0)
        ParseExports(export_rva, export_size);

    LogDebug("PE '{}': {} sections, base {:#x}, size {:#x}, {}",
             name_, sections_.size(), preferred_base_, size_of_image_,
             is_64bit_ ? "x64" : "x86");
    return true;
}

void PeImage::ParseExports(std::uint32_t dir_rva, std::uint32_t) {
    struct Header {
        std::uint32_t ordinal_base{}, address_count{}, name_count{};
        std::uint32_t address_rva{}, name_rva{}, ordinal_rva{};
    } header;

    if (ReadRva(dir_rva + 16, &header.ordinal_base,  4) != 4) return;
    if (ReadRva(dir_rva + 20, &header.address_count, 4) != 4) return;
    if (ReadRva(dir_rva + 24, &header.name_count,    4) != 4) return;
    if (ReadRva(dir_rva + 28, &header.address_rva,   4) != 4) return;
    if (ReadRva(dir_rva + 32, &header.name_rva,      4) != 4) return;
    if (ReadRva(dir_rva + 36, &header.ordinal_rva,   4) != 4) return;

    // No real export table has millions of entries, and trusting the field lets a
    // malformed file drive an enormous allocation.
    constexpr std::uint32_t kMaxExports = 1u << 20;
    if (header.name_count > kMaxExports) return;

    exports_.reserve(header.name_count);
    for (std::uint32_t i = 0; i < header.name_count; ++i) {
        std::uint32_t name_str_rva{};
        std::uint16_t ordinal{};
        if (ReadRva(header.name_rva + i * 4, &name_str_rva, 4) != 4) break;
        if (ReadRva(header.ordinal_rva + i * 2, &ordinal, 2) != 2) break;

        std::uint32_t func_rva{};
        if (ReadRva(header.address_rva + static_cast<std::uint32_t>(ordinal) * 4,
                    &func_rva, 4) != 4) break;

        std::string name;
        for (std::uint32_t k = 0; k < 512; ++k) {
            char c{};
            if (ReadRva(name_str_rva + k, &c, 1) != 1 || c == '\0') break;
            name.push_back(c);
        }
        if (name.empty()) continue;

        exports_.push_back(PeExport{std::move(name), func_rva,
                                    static_cast<std::uint16_t>(ordinal + header.ordinal_base)});
    }
}

const PeSection* PeImage::SectionForRva(std::uint32_t rva) const {
    const auto it = std::find_if(sections_.begin(), sections_.end(), [&](const PeSection& s) {
        return rva >= s.rva && rva < s.rva + std::max(s.virtual_size, s.raw_size);
    });
    return it == sections_.end() ? nullptr : &*it;
}

const PeSection* PeImage::SectionNamed(std::string_view name) const {
    const auto it = std::find_if(sections_.begin(), sections_.end(),
                                 [&](const PeSection& s) { return s.name == name; });
    return it == sections_.end() ? nullptr : &*it;
}

std::size_t PeImage::ReadRva(std::uint32_t rva, void* out, std::size_t size) const {
    auto* dest = static_cast<std::uint8_t*>(out);
    std::size_t done = 0;

    while (done < size) {
        const std::uint32_t cur = rva + static_cast<std::uint32_t>(done);

        // PE headers live below the first section, mapped at file offset == RVA. Without
        // this, DOS/NT header reads fall through to the section table and fail.
        if (cur < size_of_headers_) {
            const std::size_t want = std::min<std::size_t>(size - done, size_of_headers_ - cur);
            const std::size_t available = cur < bytes_.size()
                ? std::min<std::size_t>(want, bytes_.size() - cur)
                : 0;
            if (available == 0) break;

            std::memcpy(dest + done, bytes_.data() + cur, available);
            done += available;
            if (available < want) break;
            continue;
        }

        const PeSection* section = SectionForRva(cur);
        if (!section) break;

        const std::uint32_t offset_in_section = cur - section->rva;
        const std::uint32_t section_span = std::max(section->virtual_size, section->raw_size);
        const std::size_t   want = std::min<std::size_t>(size - done, section_span - offset_in_section);

        if (offset_in_section < section->raw_size) {
            // Backed by file bytes, possibly only partially.
            const std::size_t available = std::min<std::size_t>(
                want, section->raw_size - offset_in_section);
            const std::size_t file_offset = section->raw_offset + offset_in_section;
            if (file_offset + available > bytes_.size()) break;

            std::memcpy(dest + done, bytes_.data() + file_offset, available);
            // Any remainder of this span lies past raw data: zero-fill like the loader.
            if (available < want) std::memset(dest + done + available, 0, want - available);
        } else {
            // Entirely past raw data — uninitialised section tail.
            std::memset(dest + done, 0, want);
        }

        done += want;
        if (want == 0) break;
    }
    return done;
}

std::vector<RegionInfo> ExecutableRanges(IMemorySource& memory) {
    std::vector<RegionInfo> ranges;

    for (const auto& module : memory.Modules()) {
        // One page. Less truncates the section table of a module with many sections; more
        // is wasted.
        std::vector<std::uint8_t> headers(0x1000);
        const std::size_t got = memory.Read(module.base, headers.data(), headers.size());
        if (got < 0x200) continue;
        headers.resize(got);

        std::uint16_t dos_magic{};
        std::int32_t  pe_offset{};
        if (!At(headers, 0, dos_magic) || dos_magic != kDosMagic) continue;
        if (!At(headers, 0x3C, pe_offset) || pe_offset < 0) continue;

        const auto pe = static_cast<std::size_t>(pe_offset);
        std::uint32_t pe_magic{};
        if (!At(headers, pe, pe_magic) || pe_magic != kPeMagic) continue;

        std::uint16_t section_count{}, optional_size{};
        if (!At(headers, pe + 6, section_count) || !At(headers, pe + 20, optional_size))
            continue;

        const std::size_t table = pe + 24 + optional_size;
        for (std::uint16_t i = 0; i < section_count; ++i) {
            const std::size_t entry = table + static_cast<std::size_t>(i) * 40;
            if (entry + 40 > headers.size()) break;

            std::uint32_t virtual_size{}, rva{}, raw_size{}, characteristics{};
            At(headers, entry + 8,  virtual_size);
            At(headers, entry + 12, rva);
            At(headers, entry + 16, raw_size);
            At(headers, entry + 36, characteristics);

            if (!(characteristics & kSecMemExecute)) continue;

            const std::uint32_t span = std::max(virtual_size, raw_size);
            if (span == 0) continue;

            ranges.push_back(RegionInfo{module.base + rva, span,
                                        ProtectFromCharacteristics(characteristics), true});
        }
    }

    std::sort(ranges.begin(), ranges.end(), [](const RegionInfo& a, const RegionInfo& b) {
        return Raw(a.base) < Raw(b.base);
    });
    return ranges;
}

} // namespace zircon::core
