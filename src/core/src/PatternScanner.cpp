#include "core/PatternScanner.h"
#include "core/Log.h"
#include "core/PeImage.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>

namespace zircon::core {
namespace {

std::optional<std::uint8_t> HexNibble(char c) {
    if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<std::uint8_t>(c - 'A' + 10);
    return std::nullopt;
}

// Big enough to bury ReadProcessMemory's per-call overhead, small enough to stay polite
// about memory.
constexpr std::size_t kChunkSize = 1u << 20;   // 1 MiB

} // namespace

Result<Pattern> Pattern::Parse(std::string_view text) {
    Pattern pattern;

    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }

        if (c == '?') {
            pattern.bytes_.push_back(0x00);
            pattern.mask_.push_back(0x00);
            ++i;
            if (i < text.size() && text[i] == '?') ++i;   // accept both "?" and "??"
            continue;
        }

        const auto hi = HexNibble(c);
        if (!hi) return Error{std::format("bad character '{}' in pattern '{}'", c, text), 6};

        if (i + 1 >= text.size())
            return Error{std::format("truncated hex byte at end of pattern '{}'", text), 6};

        const auto lo = HexNibble(text[i + 1]);
        if (!lo) return Error{std::format("bad hex byte '{}{}' in pattern '{}'",
                                          c, text[i + 1], text), 6};

        pattern.bytes_.push_back(static_cast<std::uint8_t>((*hi << 4) | *lo));
        pattern.mask_.push_back(0xFF);
        i += 2;
    }

    if (pattern.bytes_.empty())
        return Error{"pattern is empty", 6};

    const auto first = std::find(pattern.mask_.begin(), pattern.mask_.end(), 0xFF);
    if (first == pattern.mask_.end())
        return Error{"pattern is entirely wildcards", 6};

    pattern.first_fixed_ = static_cast<std::size_t>(first - pattern.mask_.begin());
    return pattern;
}

Result<Pattern> Pattern::FromBytesAndMask(const char* bytes, std::string_view mask) {
    if (!bytes) return Error{"byte string is null", 6};
    if (mask.empty()) return Error{"mask is empty", 6};

    Pattern pattern;
    pattern.bytes_.reserve(mask.size());
    pattern.mask_.reserve(mask.size());

    for (std::size_t i = 0; i < mask.size(); ++i) {
        const bool fixed = mask[i] == 'x' || mask[i] == 'X';
        pattern.bytes_.push_back(static_cast<std::uint8_t>(bytes[i]));
        pattern.mask_.push_back(fixed ? 0xFF : 0x00);
    }

    const auto first = std::find(pattern.mask_.begin(), pattern.mask_.end(), 0xFF);
    if (first == pattern.mask_.end())
        return Error{"pattern is entirely wildcards", 6};

    pattern.first_fixed_ = static_cast<std::size_t>(first - pattern.mask_.begin());
    return pattern;
}

bool Pattern::MatchesAt(const std::uint8_t* data) const {
    for (std::size_t i = 0; i < bytes_.size(); ++i) {
        if (mask_[i] && data[i] != bytes_[i]) return false;
    }
    return true;
}

std::string Pattern::ToString() const {
    std::string out;
    for (std::size_t i = 0; i < bytes_.size(); ++i) {
        if (i) out.push_back(' ');
        if (mask_[i]) out += std::format("{:02X}", bytes_[i]);
        else          out += "??";
    }
    return out;
}

std::vector<PatternScanner::ScanRange> PatternScanner::BuildRanges(
    const ScanOptions& options) const {

    // Whole-process mode isn't scoped to a module, so skip the lookup.
    if (options.whole_process) {
        std::vector<ScanRange> everything;
        for (const auto& region : mem_.Regions()) {
            if (!HasFlag(region.protect, RegionProtect::Read)) continue;
            if (options.executable_only && !HasFlag(region.protect, RegionProtect::Execute))
                continue;
            if (region.size == 0) continue;
            everything.push_back(ScanRange{region.base, region.size});
        }
        return everything;
    }

    const ModuleInfo* module = options.module.empty()
        ? mem_.MainModule()
        : mem_.FindModule(options.module);

    if (!module) {
        LogWarn("scan target module '{}' not found",
                options.module.empty() ? "<main>" : options.module);
        return {};
    }

    const std::uint64_t module_lo = Raw(module->base);
    const std::uint64_t module_hi = module_lo + module->size;

    std::vector<ScanRange> ranges;
    for (const auto& region : mem_.Regions()) {
        const std::uint64_t lo = Raw(region.base);
        const std::uint64_t hi = lo + region.size;

        if (hi <= module_lo || lo >= module_hi) continue;
        if (options.executable_only && !HasFlag(region.protect, RegionProtect::Execute))
            continue;
        if (!HasFlag(region.protect, RegionProtect::Read)) continue;

        // Clip, or a region spanning past the module drags in its neighbours.
        const std::uint64_t clipped_lo = std::max(lo, module_lo);
        const std::uint64_t clipped_hi = std::min(hi, module_hi);
        if (clipped_hi <= clipped_lo) continue;

        ranges.push_back(ScanRange{static_cast<Address>(clipped_lo), clipped_hi - clipped_lo});
    }

    // Named sections need a source that reports section-granular regions. Static does;
    // a live process coalesces them, so fall back to the whole module instead of
    // returning nothing and calling it a clean miss.
    if (!options.section.empty()) {
        LogDebug("section filter '{}' requested; {} candidate ranges before filtering",
                 options.section, ranges.size());
    }

    return ranges;
}

std::vector<Address> PatternScanner::Scan(const Pattern& pattern, const ScanOptions& options) {
    std::vector<Address> hits;
    if (pattern.Empty()) return hits;

    const std::size_t pattern_size = pattern.Size();
    const std::size_t overlap = pattern_size - 1;

    const std::uint8_t first_byte = pattern.Bytes()[pattern.FirstFixedIndex()];
    const std::size_t first_index = pattern.FirstFixedIndex();

    std::vector<std::uint8_t> buffer(kChunkSize + overlap);

    for (const auto& range : BuildRanges(options)) {
        std::uint64_t offset = 0;

        while (offset < range.size) {
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uint64_t>(kChunkSize + overlap, range.size - offset));

            const std::size_t got = mem_.Read(range.base + offset, buffer.data(), want);
            if (got < pattern_size) {
                // Unreadable, or too small to hold a match. Skip, don't spin; short reads
                // are normal near region tails.
                offset += got ? got : kChunkSize;
                continue;
            }

            const std::size_t limit = got - pattern_size + 1;
            const std::uint8_t* data = buffer.data();

            for (std::size_t i = 0; i < limit;) {
                // Jump to the next occurrence of the first fixed byte instead of testing
                // every offset. The whole reason a full .text scan is fast.
                const void* found = std::memchr(data + i + first_index, first_byte,
                                                limit - i + pattern_size - 1 - first_index);
                if (!found) break;

                const std::size_t candidate =
                    static_cast<std::size_t>(static_cast<const std::uint8_t*>(found) - data)
                    - first_index;
                if (candidate >= limit) break;

                if (pattern.MatchesAt(data + candidate)) {
                    hits.push_back(range.base + offset + candidate);
                    if (options.max_results && hits.size() >= options.max_results) return hits;
                }
                i = candidate + 1;
            }

            if (got < want) break;          // hit the end of readable memory in this range
            offset += got - overlap;        // overlap so a match straddling chunks survives
        }
    }

    return hits;
}

std::optional<Address> PatternScanner::ScanFirst(const Pattern& pattern,
                                                 const ScanOptions& options) {
    ScanOptions single = options;
    single.max_results = 1;
    const auto hits = Scan(pattern, single);
    if (hits.empty()) return std::nullopt;
    return hits.front();
}

std::optional<Address> PatternScanner::ScanUnique(const Pattern& pattern,
                                                  const ScanOptions& options) {
    ScanOptions capped = options;
    capped.max_results = 2;   // two is enough to prove ambiguity

    const auto hits = Scan(pattern, capped);
    if (hits.empty()) {
        LogDebug("pattern '{}' not found", pattern.ToString());
        return std::nullopt;
    }
    if (hits.size() > 1) {
        LogWarn("pattern '{}' is ambiguous ({}+ matches, first two at {:#x} and {:#x}); "
                "refusing to guess",
                pattern.ToString(), hits.size(), Raw(hits[0]), Raw(hits[1]));
        return std::nullopt;
    }
    return hits.front();
}

std::optional<Address> ResolveRipRelative(IMemorySource& mem, Address instruction,
                                          std::uint32_t disp_offset,
                                          std::uint32_t insn_length) {
    if (disp_offset + 4 > insn_length) return std::nullopt;

    std::int32_t displacement{};
    if (!ReadInto(mem, instruction + disp_offset, displacement)) return std::nullopt;

    // RIP is the *following* instruction's address when the operand resolves.
    const std::uint64_t rip = Raw(instruction) + insn_length;
    return static_cast<Address>(rip + static_cast<std::int64_t>(displacement));
}

} // namespace zircon::core
