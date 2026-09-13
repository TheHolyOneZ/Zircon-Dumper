#pragma once

#include "core/Types.h"

#include <span>
#include <vector>

namespace zircon::core {

struct PeSection {
    std::string   name;
    std::uint32_t rva{};
    std::uint32_t virtual_size{};
    std::uint32_t raw_offset{};
    std::uint32_t raw_size{};
    RegionProtect protect{RegionProtect::None};
};

struct PeExport {
    std::string   name;
    std::uint32_t rva{};
    std::uint16_t ordinal{};
};

// A PE parsed straight from disk. Deliberately does not use windows.h structures: we
// parse files that are never loaded by the OS loader, so relying on loader-validated
// layout would be wrong, and explicit field reads make malformed-file handling obvious.
class PeImage {
public:
    static Result<PeImage> FromFile(std::string_view path);
    static Result<PeImage> FromBuffer(std::vector<std::uint8_t> bytes, std::string name);

    const std::string& Name() const { return name_; }
    std::uint64_t PreferredBase() const { return preferred_base_; }
    std::uint32_t SizeOfImage()   const { return size_of_image_; }
    std::uint32_t EntryPointRva() const { return entry_point_rva_; }
    std::uint32_t SizeOfHeaders() const { return size_of_headers_; }
    bool          Is64Bit()       const { return is_64bit_; }

    std::span<const PeSection> Sections() const { return sections_; }
    std::span<const PeExport>  Exports()  const { return exports_; }

    const PeSection* SectionForRva(std::uint32_t rva) const;
    const PeSection* SectionNamed(std::string_view name) const;

    // Reads as if the image were mapped at PreferredBase(). Bytes past raw_size inside
    // a section are zero-filled, matching what the loader does for .bss-style tails.
    // RVAs below SizeOfHeaders resolve to the PE headers, which sit before the first
    // section and are mapped by the loader at file offset == RVA.
    std::size_t ReadRva(std::uint32_t rva, void* out, std::size_t size) const;

    const std::vector<std::uint8_t>& Raw() const { return bytes_; }

private:
    Result<bool> Parse();
    void ParseExports(std::uint32_t dir_rva, std::uint32_t dir_size);

    std::vector<std::uint8_t> bytes_;
    std::string               name_;
    std::vector<PeSection>    sections_;
    std::vector<PeExport>     exports_;
    std::uint64_t             preferred_base_{};
    std::uint32_t             size_of_image_{};
    std::uint32_t             entry_point_rva_{};
    std::uint32_t             size_of_headers_{};
    bool                      is_64bit_{false};
};

// The executable address ranges of every loaded module, read from the PE section headers
// in memory rather than from page protection.
//
// Page protection is not a usable signal across all sources: a minidump captured without
// MiniDumpWithFullMemoryInfo records none, so the Dump provider reports every region as
// read/write/execute and any "is this code?" test becomes vacuously true. Section headers
// are present in the mapped image itself, so this answers the same question identically
// for a live process, a dump and a static image.
std::vector<RegionInfo> ExecutableRanges(class IMemorySource& memory);

} // namespace zircon::core
