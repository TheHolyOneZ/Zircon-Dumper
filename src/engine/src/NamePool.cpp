#include "engine/NamePool.h"
#include "engine/Hooks.h"
#include "core/Log.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <format>

namespace zircon::engine {
namespace {

using core::Address;
using core::IsNull;
using core::Raw;

// FName index 0, and the first name the engine ever interns, so block 0 always begins with
// its entry. Everything below is measured from it.
constexpr std::string_view kAnchorName = "None";

// Where the characters can begin relative to the entry start. 2 is the ordinary
// FNameEntryHeader; 6 is a case-preserving build, which stores a comparison id first.
constexpr std::uint32_t kCharOffsets[] = {2, 6};

constexpr std::size_t kMaxNameLength = 1024;

bool Readable(core::IMemorySource& memory, Address addr, std::size_t size = 8) {
    if (IsNull(addr) || Raw(addr) < 0x10000 || Raw(addr) >= 0x7FFFFFFFFFFFull) return false;
    std::uint8_t probe[64];
    const std::size_t want = std::min(size, sizeof(probe));
    return memory.Read(addr, probe, want) == want;
}

bool DeriveEntryFormat(const std::uint8_t* bytes, std::size_t size, NamePoolInfo& out) {
    for (const std::uint32_t char_offset : kCharOffsets) {
        if (char_offset + kAnchorName.size() > size) continue;
        if (std::memcmp(bytes + char_offset, kAnchorName.data(), kAnchorName.size()) != 0)
            continue;

        // Header sits in the two bytes immediately before the characters.
        std::uint16_t header{};
        std::memcpy(&header, bytes + char_offset - 2, sizeof(header));

        // Don't assume Len is the top 10 bits; find the shift that reproduces the length we
        // already know. Engine versions have moved these bits around.
        for (std::uint32_t shift = 1; shift <= 8; ++shift) {
            if ((header >> shift) != kAnchorName.size()) continue;

            out.len_shift       = shift;
            out.header_size     = 2;
            out.case_preserving = char_offset > 2;
            return true;
        }
    }
    return false;
}

} // namespace

std::uint32_t DeriveElementsPerChunk(core::IMemorySource& memory, const NamePoolInfo& probe,
                                     std::vector<std::string>& evidence);
std::optional<NamePoolInfo> FindNameEntryArray(core::IMemorySource& memory);
std::string ResolveEntryArrayName(core::IMemorySource& memory, const NamePoolInfo& pool,
                                  std::uint32_t comparison_index);

// The allocator's entry stride. FName ids count in these units.
//
// Unreal aligns pool entries to alignof(FNameEntry): 2 for a stock build, where the entry
// is a uint16 header followed by characters. Case-preserving builds put an FNameEntryId
// DisplayIndex in there too, which raises it to 4. FF7 Rebirth is one of those.
//
// Getting this wrong is the nastiest failure the pool has to offer. Assume 2 on a 4-aligned
// build and every id still resolves to *something* — byte offset id*2 lands inside the
// block either way — so you get back things that look exactly like names and aren't.
// Nothing downstream can tell. The object layout fails its end-to-end check hours later
// with no hint as to why.
//
// So: walk block 0 under each candidate and see which survives. A single entry can't tell
// you the alignment; it only shows up in where the *next* one starts. A walk that rounds to
// the wrong boundary lands mid-entry within a few steps and reads a length that isn't one.
//
// Round-tripping is no help here, in case it's tempting. Encoding a byte offset into an id
// and back is self-consistent for any stride. Only the walk is evidence.
std::uint32_t DeriveStride(core::IMemorySource& memory, const NamePoolInfo& probe,
                           std::vector<std::string>& evidence) {
    const std::uint32_t char_offset = probe.case_preserving ? 6u : 2u;

    const auto block_base = core::ReadOr<Address>(memory, probe.blocks);
    if (IsNull(block_base)) return 2;

    // Two unknowns, so search both: the alignment entries start on, and whether an entry
    // reserves a byte past its characters. That second one only shows itself when the
    // characters end exactly on the alignment, since the entry then gets padded out by a
    // whole extra unit instead of not at all. Rare enough per entry to look like noise,
    // common enough across a block to derail the walk.
    auto walk = [&](std::uint32_t stride, std::uint32_t reserved) {
        int valid = 0;
        std::uint32_t at = 0;

        for (int i = 0; i < 512; ++i) {
            std::uint16_t header{};
            if (!core::ReadInto(memory, block_base + at + (char_offset - 2), header)) break;

            const std::uint32_t length  = header >> probe.len_shift;
            const bool          is_wide = (header & 1) != 0;
            if (length == 0 || length > kMaxNameLength) break;

            if (!is_wide) {
                // Names are printable; mid-entry garbage isn't. That's the whole signal.
                std::string text(length, '\0');
                if (memory.Read(block_base + at + char_offset, text.data(), length) != length)
                    break;

                bool printable = true;
                for (const char c : text)
                    if (static_cast<unsigned char>(c) < 32 ||
                        static_cast<unsigned char>(c) > 126) { printable = false; break; }
                if (!printable) break;
            }

            ++valid;
            at += char_offset + length * (is_wide ? 2u : 1u) + reserved;
            at = (at + stride - 1) & ~(stride - 1);
        }
        return valid;
    };

    struct Result { std::uint32_t stride; std::uint32_t reserved; int valid; };
    Result best{2, 0, 0};

    // Stride 2 goes first at each reservation, so if both alignments walk the whole block we
    // keep the smaller. Safe reading: they agree on every id when entries happen to be
    // 4-aligned anyway, and 2 is what a stock build uses.
    for (const std::uint32_t reserved : {0u, 1u}) {
        for (const std::uint32_t stride : {2u, 4u}) {
            const int valid = walk(stride, reserved);
            if (valid > best.valid) best = {stride, reserved, valid};
        }
    }

    if (best.valid < 32) {
        evidence.push_back(std::format(
            "could not derive the pool stride; the best walk managed {} entries", best.valid));
        core::LogWarn("could not derive the FName pool stride; assuming 2. Names may resolve "
                      "to the wrong entries above the first block.");
        return 2;
    }

    evidence.push_back(std::format(
        "pool stride {}: walks {} entries{}", best.stride, best.valid,
        best.reserved ? " (entries reserve a byte past their characters)" : ""));
    return best.stride;
}

// How many low bits of an FName id are the offset within a block.
//
// Round-tripping is what discriminates here. Walking a block directly hands us (block, byte
// offset, string) triples with no knowledge of the encoding required. Encode those back
// into an id under a candidate width, resolve through the normal path, and you must get the
// same string; a wrong candidate names some other block and comes back as something else,
// or as nothing.
//
// Only blocks past the first can settle it. Inside block 0 the block index is zero, the
// shift cancels, and every candidate agrees. Which is how a wrong value sails through
// casual testing and then eats the long tail of a real dump.
std::uint32_t DeriveBlockOffsetBits(core::IMemorySource& memory, const NamePoolInfo& probe,
                                    std::vector<std::string>& evidence) {
    constexpr std::uint32_t kCandidates[] = {16, 17, 15, 14, 18, 13};
    const std::uint32_t char_offset = probe.case_preserving ? 6u : 2u;

    struct Fact { std::uint32_t block; std::uint32_t byte_offset; std::string text; };
    std::vector<Fact> facts;

    for (std::uint32_t block = 1; block < 6 && facts.size() < 24; ++block) {
        const auto block_base = core::ReadOr<Address>(memory, probe.blocks + block * 8);
        if (IsNull(block_base)) break;

        std::uint32_t at = 0;
        for (int entry = 0; entry < 64 && facts.size() < 24; ++entry) {
            std::uint16_t header{};
            if (!core::ReadInto(memory, block_base + at + (char_offset - 2), header)) break;

            const std::uint32_t length  = header >> probe.len_shift;
            const bool          is_wide = (header & 1) != 0;
            if (length == 0 || length > kMaxNameLength) break;

            if (!is_wide) {
                std::string text(length, '\0');
                if (memory.Read(block_base + at + char_offset, text.data(), length) != length)
                    break;

                bool printable = true;
                for (const char c : text)
                    if (static_cast<unsigned char>(c) < 32 ||
                        static_cast<unsigned char>(c) > 126) { printable = false; break; }

                // Sample, don't take every entry. Consecutive names share a block and a
                // neighbourhood, so they're nearly the same fact over and over.
                if (printable && entry % 7 == 0)
                    facts.push_back({block, at, text});
            }

            at += char_offset + length * (is_wide ? 2u : 1u);
            at = (at + 1) & ~1u;
        }
    }

    if (facts.size() < 4) {
        evidence.push_back("too few blocks to derive the block-offset bits; assumed 16");
        return 16;
    }

    for (const std::uint32_t bits : kCandidates) {
        NamePoolInfo candidate = probe;
        candidate.block_offset_bits = bits;

        bool all = true;
        for (const auto& fact : facts) {
            const std::uint32_t within = fact.byte_offset / candidate.stride;

            // Offset has to fit the width being proposed, or this block could never have
            // been encoded that way in the first place.
            if (within >= (1u << bits)) { all = false; break; }

            const std::uint32_t id = (fact.block << bits) | within;
            if (ResolveName(memory, candidate, id) != fact.text) { all = false; break; }
        }

        if (!all) continue;

        evidence.push_back(std::format(
            "block-offset bits {}: {} entries across blocks 1+ round-trip through their "
            "encoded id", bits, facts.size()));
        return bits;
    }

    evidence.push_back("no block-offset bits round-tripped; assumed 16");
    core::LogWarn("could not derive FName block-offset bits; assuming 16. Names in the "
                  "first block will be right and later ones may not be.");
    return 16;
}


// How many entries a chunk holds. 16384 is Unreal's default, but it's a template argument,
// so measure it. Same round-trip as DeriveBlockOffsetBits above, chunk 0 useless for the
// same reason.
std::uint32_t DeriveElementsPerChunk(core::IMemorySource& memory, const NamePoolInfo& probe,
                                     std::vector<std::string>& evidence) {
    constexpr std::uint32_t kCandidates[] = {16384, 8192, 32768, 65536, 4096};

    auto text_at = [&](Address entry) -> std::string {
        if (!Readable(memory, entry, 16)) return {};

        const auto index = core::ReadOr<std::uint32_t>(memory, entry + probe.entry_index);
        if (index & 1) return {};                       // wide; skipped for this purpose

        char buffer[128]{};
        if (memory.Read(entry + probe.entry_chars, buffer, sizeof(buffer) - 1) == 0) return {};

        std::string out;
        for (const char c : buffer) {
            if (c == 0) break;
            if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126)
                return {};
            out.push_back(c);
        }
        return out;
    };

    struct Fact { std::uint32_t chunk; std::uint32_t within; std::string text; };
    std::vector<Fact> facts;

    for (std::uint32_t chunk = 1; chunk < 5 && facts.size() < 16; ++chunk) {
        const auto chunk_base = core::ReadOr<Address>(memory, probe.blocks + chunk * 8);
        if (IsNull(chunk_base)) break;

        for (std::uint32_t within = 0; within < 512 && facts.size() < 16; within += 37) {
            const auto entry = core::ReadOr<Address>(memory, chunk_base + within * 8);
            if (IsNull(entry)) continue;

            const std::string text = text_at(entry);
            if (text.size() >= 3) facts.push_back({chunk, within, text});
        }
    }

    if (facts.size() < 4) {
        evidence.push_back("too few chunks to derive the entries per chunk; assumed 16384");
        return 16384;
    }

    for (const std::uint32_t candidate : kCandidates) {
        NamePoolInfo trial = probe;
        trial.elements_per_chunk = candidate;

        bool all = true;
        for (const auto& fact : facts) {
            if (fact.within >= candidate) { all = false; break; }
            if (ResolveName(memory, trial, fact.chunk * candidate + fact.within) != fact.text) {
                all = false;
                break;
            }
        }
        if (!all) continue;

        evidence.push_back(std::format(
            "{} entries per chunk: {} entries beyond chunk 0 round-trip through their id",
            candidate, facts.size()));
        return candidate;
    }

    evidence.push_back("no entries-per-chunk round-tripped; assumed 16384");
    core::LogWarn("could not derive the TNameEntryArray chunk size; assuming 16384. Names "
                  "in the first chunk will be right and later ones may not be.");
    return 16384;
}


// --- TNameEntryArray, UE 4.22 and earlier ----------------------------------------------
//
// Two levels of pointers rather than blocks of packed bytes: a chunk table, each chunk an
// array of FNameEntry*, an id split into (chunk, index within chunk). No length header on
// the entry; the wide flag lives in the entry's own index field and the characters are
// NUL-terminated.
//
// Same anchor as the modern pool, since the same fact holds: chunk 0's first entry is
// "None". Everything else gets measured from it.
std::optional<NamePoolInfo> FindNameEntryArray(core::IMemorySource& memory) {
    const auto* main_module = memory.MainModule();
    if (!main_module) return std::nullopt;

    const std::uint64_t module_lo = Raw(main_module->base);
    const std::uint64_t module_hi = module_lo + main_module->size;

    // How far in to look for "None". Searched, not assumed: an entry starts with an index
    // and a hash link, but the packing is a build detail. 4.22 puts the characters at +0xc,
    // where reading the struct declaration would tell you +0x10.
    constexpr int kMaxCharsOffset = 0x20;

    // A build holding its names as UTF-16 writes "None" as 4E 00 6F 00 6E 00 65 00, and an
    // ANSI-only anchor then finds precisely nothing.
    static const char kWideNone[] = "N\0o\0n\0e\0\0";

    // `table` is the address of Chunks[0].
    auto consider = [&](Address table) -> std::optional<NamePoolInfo> {
        const auto chunk = core::ReadOr<Address>(memory, table);
        if (!Readable(memory, chunk, 8)) return std::nullopt;

        const auto entry = core::ReadOr<Address>(memory, chunk);
        if (!Readable(memory, entry, 16)) return std::nullopt;

        char probe[kMaxCharsOffset + 12]{};
        if (memory.Read(entry, probe, sizeof(probe)) < kMaxCharsOffset) return std::nullopt;

        int chars_at = -1;
        for (int at = 2; at <= kMaxCharsOffset; at += 2) {
            if (std::memcmp(probe + at, "None", 5) == 0 ||
                std::memcmp(probe + at, kWideNone, 9) == 0) {
                chars_at = at;
                break;
            }
        }
        if (chars_at < 0) return std::nullopt;

        NamePoolInfo info;
        info.blocks       = table;
        info.entry_array  = true;
        info.chunked_pool = false;
        info.entry_chars  = chars_at;
        info.entry_index  = 0;

        int plausible_chunks = 1;
        for (int i = 1; i < 8; ++i) {
            const auto next = core::ReadOr<Address>(memory, table + i * 8);
            if (IsNull(next)) break;
            if (!Readable(memory, next, 8)) break;
            if (!Readable(memory, core::ReadOr<Address>(memory, next), 16)) break;
            ++plausible_chunks;
        }

        // "None" sitting somewhere inside a blob is not enough, and this is the sixth time
        // in this project that an anchor alone has picked the wrong thing: the first
        // candidate accepted here resolved id 0 to "None" and then gave "r2DFloat" and
        // "otify" for everything after it, having matched a fragment of unrelated memory.
        //
        // The independent check is that the *table* works: walking the first ids must yield
        // whole, distinct, printable names. A coincidence does not survive that.
        std::set<std::string> distinct;
        int resolved = 0;
        for (std::uint32_t id = 0; id < 96; ++id) {
            const std::string name = ResolveEntryArrayName(memory, info, id);
            if (name.size() < 2) continue;
            ++resolved;
            distinct.insert(name);
        }
        if (resolved < 48 || distinct.size() < 32) return std::nullopt;

        info.elements_per_chunk = DeriveElementsPerChunk(memory, info, info.evidence);
        info.confidence = std::min(0.95f, 0.55f + 0.05f * plausible_chunks);

        info.evidence.push_back(std::format(
            "chunk 0 entry 0 is the \"None\" entry, characters at +{:#x}", chars_at));
        info.evidence.push_back(std::format(
            "{} of the first 96 ids resolve to {} distinct names", resolved, distinct.size()));
        info.evidence.push_back(std::format(
            "{} consecutive plausible chunk pointers", plausible_chunks));
        info.evidence.push_back("TNameEntryArray: two-level pointer table, no length header");

        return info;
    };

    for (const auto& region : memory.Regions()) {
        const std::uint64_t lo = Raw(region.base);
        if (lo < module_lo || lo >= module_hi) continue;
        if (!core::HasFlag(region.protect, core::RegionProtect::Write)) continue;
        if (!core::HasFlag(region.protect, core::RegionProtect::Read)) continue;

        for (std::uint64_t offset = 0; offset + 8 <= region.size; offset += 8) {
            const Address slot = region.base + offset;

            // Both shapes exist. Classically FNameEntryArray holds its chunk pointers
            // inline, so the global *is* Chunks[0] and the slot is the table. 4.22 puts the
            // chunk array on the heap and keeps only a pointer to it in the image, one more
            // hop. Look for the first shape only and such a build yields nothing, with no
            // hint as to why.
            if (auto inline_array = consider(slot)) {
                core::LogInfo("name entry array at {:#x} ({} entries per chunk, chars at "
                              "+{:#x})", Raw(inline_array->blocks),
                              inline_array->elements_per_chunk, inline_array->entry_chars);
                return inline_array;
            }

            const auto indirect = core::ReadOr<Address>(memory, slot);
            if (!Readable(memory, indirect, 8)) continue;

            if (auto heap_array = consider(indirect)) {
                heap_array->evidence.push_back(std::format(
                    "chunk array is heap-allocated; the image holds a pointer to it at {:#x}",
                    Raw(slot)));
                core::LogInfo("name entry array at {:#x} via {:#x} ({} entries per chunk, "
                              "chars at +{:#x})", Raw(heap_array->blocks), Raw(slot),
                              heap_array->elements_per_chunk, heap_array->entry_chars);
                return heap_array;
            }
        }
    }

    return std::nullopt;
}

std::optional<NamePoolInfo> FindNamePool(core::IMemorySource& memory,
                                         const EngineProfile& profile) {
    if (!memory.Caps().live_objects) {
        core::LogWarn("name pool lookup needs live memory; the pool is heap-allocated at "
                      "runtime and does not exist in a PE on disk");
        return std::nullopt;
    }

    const auto* main_module = memory.MainModule();
    if (!main_module) return std::nullopt;

    const std::uint64_t module_lo = Raw(main_module->base);
    const std::uint64_t module_hi = module_lo + main_module->size;

    std::vector<NamePoolInfo> found;

    // A resolver's candidate gets checked first, but by exactly the same rule as a scanned
    // one: block 0 must begin with the "None" entry. Pass and we skip the scan, since
    // there's nothing left to establish. Fail and we drop it and scan anyway, so a wrong
    // hint costs a little time and nothing else.
    if (GlobalCandidates hint; ResolveGlobals(memory, hint) && !IsNull(hint.name_pool)) {
        const auto block = core::ReadOr<Address>(memory, hint.name_pool);

        std::uint8_t bytes[32]{};
        NamePoolInfo info;
        if (Readable(memory, block, 16) &&
            memory.Read(block, bytes, sizeof(bytes)) == sizeof(bytes) &&
            DeriveEntryFormat(bytes, sizeof(bytes), info)) {

            info.blocks       = hint.name_pool;
            info.chunked_pool = true;
            info.stride       = 2;
            info.confidence   = 0.95f;
            info.evidence.push_back("address supplied by a resolver hook");
            info.evidence.push_back(std::format(
                "block 0 begins with the \"None\" entry, characters at +{}",
                info.case_preserving ? 6 : 2));

            info.stride            = DeriveStride(memory, info, info.evidence);
            info.block_offset_bits = DeriveBlockOffsetBits(memory, info, info.evidence);

            core::LogInfo("name pool blocks at {:#x} (from a resolver hook), block-offset "
                          "bits {}", Raw(info.blocks), info.block_offset_bits);
            return info;
        }
        core::LogWarn("the resolver's FNamePool address did not validate; scanning instead");
    }

    for (const auto& region : memory.Regions()) {
        const std::uint64_t lo = Raw(region.base);
        if (lo < module_lo || lo >= module_hi) continue;
        if (!core::HasFlag(region.protect, core::RegionProtect::Write)) continue;
        if (!core::HasFlag(region.protect, core::RegionProtect::Read)) continue;

        for (std::uint64_t offset = 0; offset + 8 <= region.size; offset += 8) {
            const Address slot = region.base + offset;

            const auto block = core::ReadOr<Address>(memory, slot);
            if (!Readable(memory, block, 16)) continue;

            std::uint8_t bytes[32]{};
            if (memory.Read(block, bytes, sizeof(bytes)) != sizeof(bytes)) continue;

            NamePoolInfo info;
            if (!DeriveEntryFormat(bytes, sizeof(bytes), info)) continue;

            info.blocks       = slot;
            info.chunked_pool = true;
            info.stride       = 2;

            // A real pool has more than one block once a game is loaded, so the following
            // slots must be plausible block pointers too. Rejects a lone "None" sitting
            // behind some unrelated pointer.
            int plausible_blocks = 1;
            for (int i = 1; i < 8; ++i) {
                const auto next = core::ReadOr<Address>(memory, slot + i * 8);
                if (IsNull(next)) break;
                if (!Readable(memory, next, 8)) break;
                ++plausible_blocks;
            }

            info.confidence = std::min(0.95f, 0.55f + 0.05f * plausible_blocks);
            info.evidence.push_back(std::format(
                "block 0 begins with the \"None\" entry, characters at +{}",
                info.case_preserving ? 6 : 2));
            info.evidence.push_back(std::format(
                "length packed as header >> {}", info.len_shift));
            info.evidence.push_back(std::format(
                "{} consecutive plausible block pointers", plausible_blocks));
            if (info.case_preserving)
                info.evidence.push_back("case-preserving entry format");

            found.push_back(std::move(info));
        }
    }

    if (found.empty()) {
        // No FNamePool means 4.22 or earlier, where the pool is a different shape entirely.
        // Falling through to the older layout is how the engine era ends up discovered
        // rather than configured.
        core::LogDebug("no FNamePool; trying TNameEntryArray");
        if (auto older = FindNameEntryArray(memory)) return older;

        core::LogWarn("no FName pool found, in either layout");
        return std::nullopt;
    }

    std::sort(found.begin(), found.end(),
              [](const NamePoolInfo& a, const NamePoolInfo& b) {
                  return a.confidence > b.confidence;
              });

    if (found.size() > 1) {
        core::LogDebug("{} name-pool candidates; taking the strongest", found.size());
        found.front().evidence.push_back(
            std::format("{} candidates found; selected the strongest", found.size()));
    }

    found.front().stride = DeriveStride(memory, found.front(), found.front().evidence);
    found.front().block_offset_bits =
        DeriveBlockOffsetBits(memory, found.front(), found.front().evidence);

    core::LogInfo("name pool blocks at {:#x} (confidence {:.2f}, block-offset bits {})",
                  Raw(found.front().blocks), found.front().confidence,
                  found.front().block_offset_bits);

    (void)profile;
    return found.front();
}


// Characters here are NUL-terminated. With no length prefix the only bounds we get are the
// buffer and the printable check, and failing either returns an empty entry.
std::string ResolveEntryArrayName(core::IMemorySource& memory, const NamePoolInfo& pool,
                                  std::uint32_t comparison_index) {
    if (pool.elements_per_chunk == 0) return {};

    const std::uint32_t chunk  = comparison_index / pool.elements_per_chunk;
    const std::uint32_t within = comparison_index % pool.elements_per_chunk;
    if (chunk > 512) return {};

    const auto chunk_base = core::ReadOr<Address>(memory, pool.blocks + chunk * 8);
    if (IsNull(chunk_base)) return {};

    const auto entry = core::ReadOr<Address>(memory, chunk_base + within * 8);
    if (IsNull(entry)) return {};

    char buffer[kMaxNameLength * 2 + 2]{};
    const std::size_t got = memory.Read(entry + pool.entry_chars, buffer, sizeof(buffer) - 2);
    if (got < 2) return {};

    // Width comes from the bytes.
    //
    // Unreal does keep a wide bit in the entry's index, but *where* it lives moved between
    // builds, and a wrong guess quietly hands back half a string. The bytes have no such
    // ambiguity: a UTF-16 name's second byte is zero where an ANSI name's is another
    // character. The two readings coincide only for single-character names, and there they
    // agree on the character anyway.
    const bool is_wide = buffer[0] != 0 && buffer[1] == 0;

    std::string text;
    const std::size_t step = is_wide ? 2u : 1u;

    for (std::size_t i = 0; i + step <= got; i += step) {
        const unsigned char lo = static_cast<unsigned char>(buffer[i]);
        if (lo == 0 && (!is_wide || buffer[i + 1] == 0)) break;

        // Unprintable means this was never a name. Wrong chunk, or a stale entry pointer.
        if (lo < 32 || lo > 126) return {};
        if (is_wide && buffer[i + 1] != 0) return {};

        text.push_back(static_cast<char>(lo));
        if (text.size() > kMaxNameLength) return {};
    }
    return text;
}

std::string ResolveName(core::IMemorySource& memory, const NamePoolInfo& pool,
                        std::uint32_t comparison_index) {
    if (!pool.Valid()) return {};

    if (pool.entry_array) return ResolveEntryArrayName(memory, pool, comparison_index);

    const std::uint32_t block  = comparison_index >> pool.block_offset_bits;
    const std::uint32_t offset = comparison_index & ((1u << pool.block_offset_bits) - 1);

    // Sanity bound. The pool never gets anywhere near this many blocks, and without it a
    // wild index reads far outside the array.
    if (block > 8192) return {};

    const auto block_base = core::ReadOr<Address>(memory, pool.blocks + block * 8);
    if (IsNull(block_base)) return {};

    const Address entry = block_base + static_cast<std::uint64_t>(offset) * pool.stride;

    const std::uint32_t char_offset = pool.case_preserving ? 6u : 2u;

    // A decoder hands back plain entry bytes, so the parsing below is the same either way.
    // An encrypted pool changes how the bytes are obtained, not what they mean. Length
    // bound and printable test still apply, which is what stops a broken decoder from
    // spraying noise through the dump.
    std::uint8_t decoded[2 + kMaxNameLength * 2]{};
    std::size_t  decoded_size = 0;

    if (const auto& decoder = GetNameEntryDecoder(); decoder)
        decoded_size = decoder(memory, entry + (char_offset - 2), decoded, sizeof(decoded));

    const bool from_decoder = decoded_size >= 2;

    auto read_at = [&](std::uint32_t at, void* out, std::size_t size) -> bool {
        if (from_decoder) {
            // `at` is relative to the header, which is where we pointed the decoder.
            const std::size_t from = at - (char_offset - 2);
            if (from + size > decoded_size) return false;
            std::memcpy(out, decoded + from, size);
            return true;
        }
        return memory.Read(entry + at, out, size) == size;
    };

    std::uint16_t header{};
    if (!read_at(char_offset - 2, &header, sizeof(header))) return {};

    const std::uint32_t length  = header >> pool.len_shift;
    const bool          is_wide = (header & 1) != 0;

    if (length == 0 || length > kMaxNameLength) return {};

    if (!is_wide) {
        std::string text(length, '\0');
        if (!read_at(char_offset, text.data(), length)) return {};

        // Wrong offset produces binary noise; returning it as a name corrupts every
        // emitter downstream.
        for (const char c : text)
            if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126)
                return {};
        return text;
    }

    std::vector<std::uint16_t> wide(length);
    if (!read_at(char_offset, wide.data(), length * 2)) return {};

    std::string text;
    text.reserve(length);
    for (const std::uint16_t c : wide) {
        if (c == 0 || c > 126) return {};
        text.push_back(static_cast<char>(c));
    }
    return text;
}

std::string ResolveFName(core::IMemorySource& memory, const NamePoolInfo& pool,
                         std::uint32_t comparison_index, std::int32_t number) {
    std::string text = ResolveName(memory, pool, comparison_index);
    if (text.empty()) return {};

    // FName stores Number one higher than it displays, so that 0 can mean "no suffix".
    if (number > 0) text += std::format("_{}", number - 1);
    return text;
}

} // namespace zircon::engine
