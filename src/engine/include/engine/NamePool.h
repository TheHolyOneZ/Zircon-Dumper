#pragma once

#include "core/MemorySource.h"
#include "engine/EngineProfile.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace zircon::engine {

// The FName pool. Every name in the engine is an index into this, so nothing readable
// comes out of a dump until this is located.
//
// UE 4.23+ uses FNamePool: an array of block pointers, with an FNameEntry addressed by
// splitting the id into a block number and an offset within that block. Older engines
// use TNameEntryArray, which is a chunked array of entry pointers instead.
struct NamePoolInfo {
    core::Address blocks{};            // address of Blocks[0]

    // id >> this = block, id & mask = offset within it.
    //
    // 16 is Unreal's default but it is a compile-time constant (FNameBlockOffsetBits), and
    // a licensee is free to change it — FF7 Rebirth does. Get it wrong and every name id
    // below the first block boundary still resolves perfectly while every one above it
    // reads from the wrong block and says nothing, the worst possible failure shape.
    // Derived, not assumed; see DeriveBlockOffsetBits.
    std::uint32_t block_offset_bits{16};
    std::uint32_t stride{2};              // bytes per offset unit within a block
    std::uint32_t header_size{2};         // FNameEntryHeader, larger when case-preserving
    std::uint32_t len_shift{6};           // Len = header >> len_shift
    bool          case_preserving{false};
    bool          chunked_pool{true};     // FNamePool vs TNameEntryArray

    // --- TNameEntryArray, UE 4.22 and earlier -----------------------------------------
    //
    // A different shape, not a different set of offsets. FNamePool packs entries end to
    // end inside large blocks and addresses one by (block, byte offset); TNameEntryArray
    // is a two-level table of *pointers*, so an id is (chunk, index within chunk) and the
    // entry it names can be anywhere.
    //
    // Entries differ too: there is no packed length header. The wide flag lives in the
    // entry's own index field and the characters are NUL-terminated, which is why the
    // reader cannot be shared and the two are told apart here rather than downstream.
    bool          entry_array{false};

    std::uint32_t elements_per_chunk{16384};  // derived; 16384 is Unreal's default
    int           entry_chars{0};             // where the characters start in an entry
    int           entry_index{0};             // the entry's index field; bit 0 is the wide flag

    float confidence{0.0f};
    std::vector<std::string> evidence;

    bool Valid() const { return !core::IsNull(blocks); }
};

// Locates the pool by finding the entry for "None", which is always the first name the
// engine interns. Everything about the entry format — where the characters start, how
// the length is packed — is then derived from that known entry rather than assumed.
std::optional<NamePoolInfo> FindNamePool(core::IMemorySource& memory,
                                         const EngineProfile& profile);

// Resolves a raw comparison index to its string. Returns empty on any failure; callers
// treat an empty name as unresolved.
std::string ResolveName(core::IMemorySource& memory, const NamePoolInfo& pool,
                        std::uint32_t comparison_index);

// Full FName semantics: a non-zero Number renders as a "_N" suffix, where the stored
// value is one greater than the displayed one.
std::string ResolveFName(core::IMemorySource& memory, const NamePoolInfo& pool,
                         std::uint32_t comparison_index, std::int32_t number);

} // namespace zircon::engine
