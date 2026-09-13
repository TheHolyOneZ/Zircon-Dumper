#pragma once

#include "core/MemorySource.h"
#include "core/Types.h"

#include <optional>
#include <vector>

namespace zircon::core {

// A byte pattern with wildcards. Built from IDA-style text ("48 8B 05 ? ? ? ?", with
// "??" equally accepted) or from a code-style byte string plus mask ("xxx????").
class Pattern {
public:
    static Result<Pattern> Parse(std::string_view ida_style);

    // Code-style signature: a raw byte string plus a mask of 'x' (must match) and '?'
    // (wildcard). `bytes` must point to at least mask.size() bytes.
    //
    // The mask length is authoritative, and `bytes` is deliberately a pointer rather
    // than a string_view: real signatures contain 0x00 bytes, and a string_view built
    // from a literal like "\x48\x8B\x00\x00" truncates at the first NUL without a word.
    static Result<Pattern> FromBytesAndMask(const char* bytes, std::string_view mask);

    std::size_t Size() const { return bytes_.size(); }
    bool Empty() const { return bytes_.empty(); }

    std::span<const std::uint8_t> Bytes() const { return bytes_; }
    std::span<const std::uint8_t> Mask()  const { return mask_; }

    // Index of the first byte that is not a wildcard, used to skip cheaply while
    // scanning. npos when the pattern is all wildcards, which Parse rejects anyway.
    std::size_t FirstFixedIndex() const { return first_fixed_; }

    bool MatchesAt(const std::uint8_t* data) const;
    std::string ToString() const;

private:
    std::vector<std::uint8_t> bytes_;
    std::vector<std::uint8_t> mask_;   // 0xFF = must match, 0x00 = wildcard
    std::size_t               first_fixed_{0};
};

struct ScanOptions {
    // Empty module means the target's main module, which is what you almost always
    // want: scanning every loaded DLL is slow and produces matches in unrelated code.
    std::string module;

    // Empty section means every executable region of that module. Named sections
    // (".text", ".rdata", ".data") narrow it further.
    std::string section;

    bool        executable_only{true};

    // Search every committed region in the process.
    //
    // Needed for anything the engine allocates: an FName entry, an object, a container's
    // storage. `executable_only` alone does not reach them — it widens which *module*
    // regions count and the search is still inside a module, which made "scan the whole
    // process" quietly impossible and cost an afternoon of chasing a name pool that the
    // scan could not have found.
    bool        whole_process{false};

    std::size_t max_results{0};        // 0 = unlimited
};

class PatternScanner {
public:
    explicit PatternScanner(IMemorySource& mem) : mem_(mem) {}

    std::vector<Address>   Scan(const Pattern& pattern, const ScanOptions& options = {});
    std::optional<Address> ScanFirst(const Pattern& pattern, const ScanOptions& options = {});

    // Scans and requires exactly one hit. Ambiguity in a signature is a defect, not a
    // detail to paper over: two matches means the signature picks, with no warning, the
    // wrong one on the next game build. Logs and returns nullopt when ambiguous.
    std::optional<Address> ScanUnique(const Pattern& pattern, const ScanOptions& options = {});

private:
    struct ScanRange {
        Address       base{};
        std::uint64_t size{};
    };
    std::vector<ScanRange> BuildRanges(const ScanOptions& options) const;

    IMemorySource& mem_;
};

// Resolves a RIP-relative operand into the absolute address it points at. `instruction`
// is the address of the first opcode byte, `disp_offset` where the 4-byte displacement
// starts within the instruction, and `insn_length` the full instruction length, since
// RIP points at the *next* instruction.
//
// For `48 8B 05 xx xx xx xx` (mov rax, [rip+disp]) that is disp_offset 3, length 7.
std::optional<Address> ResolveRipRelative(IMemorySource& mem, Address instruction,
                                          std::uint32_t disp_offset,
                                          std::uint32_t insn_length);

} // namespace zircon::core
