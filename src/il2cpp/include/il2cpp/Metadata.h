#pragma once

// Reading global-metadata.dat without knowing its version.
//
// The table order, count and record shape change with every metadata version. That's the
// table every other IL2CPP dumper carries. We derive it from four constraints instead:
// the spans tile the file, the identifier blob is the span of NUL-terminated names, a record
// table's leading int32s all land on one of those names, and a (start, count) pair partitions
// the table it indexes. Which table it reaches is what names it.
//
// docs/IL2CPP.md has the argument and the measurements. Two traps are in Metadata.cpp at the
// point they bite.
//
// Types are not resolved here. A field's type is an index into an array in the binary, so
// metadata alone gives names, tokens and structure and nothing else. The dump says so.

#include "core/Types.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace zircon::il2cpp {

struct MetadataSpan {
    std::uint32_t offset{0};
    std::uint32_t size{0};

    bool Empty() const { return size == 0; }
};

// One (start, count) relationship found inside the type record.
struct MetadataRange {
    int           start_slot{-1};    // int32 slot holding the first index, -1 for none
    int           count_slot{-1};    // uint16 slot holding how many
    int           target{-1};        // span index the range covers
    std::uint32_t total{0};          // records in that span, from the partition
};

// Which span is which. -1 where the solver could not say, which is reported rather than
// guessed at.
struct MetadataTables {
    int strings{-1};
    int types{-1};
    int images{-1};
    int assemblies{-1};
    int fields{-1};
    int methods{-1};
    int properties{-1};
    int events{-1};
    int parameters{-1};
    int nested_types{-1};
    int interfaces{-1};
};

struct MetadataLayout {
    std::int32_t version{0};
    int          ints_per_entry{0};        // 2 = (offset,size), 3 = (offset,size,count)
    std::vector<MetadataSpan> spans;

    MetadataTables tables;

    // Record sizes, in bytes, for the tables that have one.
    int type_record{0};
    int image_record{0};
    int field_record{0};
    int method_record{0};
    int parameter_record{0};

    // Slots inside the type record. Derived, never tabulated.
    int name_slot{0};
    int namespace_slot{1};

    // Metadata tokens. ECMA-335 puts the table in the top byte -- 0x02 for a type, 0x04 for
    // a field, 0x06 for a method -- which makes the column that holds them unmistakable, and
    // makes the token a join key between a static read and a live one.
    int type_token_slot{-1};
    int field_token_slot{-1};
    int method_token_slot{-1};
    std::vector<MetadataRange> ranges;     // every partition the type record declares

    // The image record's own range into the type table. An image is the only thing that
    // declares one, which is how it gets identified in the first place.
    MetadataRange image_types;
    int image_name_slot{0};

    std::vector<std::string> evidence;

    bool Valid() const { return tables.strings >= 0 && tables.types >= 0 && type_record > 0; }

    const MetadataSpan& Span(int index) const { return spans.at(static_cast<std::size_t>(index)); }

    // The range that covers a given span, or nothing.
    const MetadataRange* RangeTo(int target) const {
        for (const auto& range : ranges)
            if (range.target == target) return &range;
        return nullptr;
    }
};

// Reads the header magic only. Cheap enough to call on anything before committing to a solve.
bool LooksLikeMetadata(std::span<const std::uint8_t> file);

// Works out the layout, or says which constraint failed.
core::Result<MetadataLayout> SolveMetadataLayout(std::span<const std::uint8_t> file);

// A NUL-terminated name out of the identifier blob. Empty when the index is out of range,
// which is a refusal rather than a crash: the index came from a file we are still proving.
std::string MetadataString(std::span<const std::uint8_t> file, const MetadataLayout& layout,
                           std::int32_t index);

} // namespace zircon::il2cpp
