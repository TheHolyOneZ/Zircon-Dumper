#pragma once

#include "core/MemorySource.h"
#include "engine/NamePool.h"
#include "engine/ObjectArray.h"
#include "engine/ObjectLayout.h"
#include "engine/StructLayout.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace zircon::engine {

// Where a UEnum keeps its entries, in whichever of the two shapes this build uses.
//
// Through UE 5.6 it is one TArray<TPair<FName, int64>>: names and values interleaved, and
// only the array needs locating because the pair layout follows from it.
//
// UE 5.7 splits them. The names and the values live in two separate arrays, each reached
// through a pointer whose low bit is set as a tag, with the entry count after them. Two
// independent 5.7 games agree on this exactly, which is what distinguishes an engine change
// from one studio's fork.
struct UEnumLayout {
    int names_array{-1};      // the interleaved array, or the names pointer when split
    int pair_stride{16};      // bytes per entry in the interleaved shape

    // 5.7 and later. values_array and count_at are relative to the enum object, like
    // names_array, so nothing downstream has to know the shape except GetEnumValues.
    bool split_arrays{false};
    int  values_array{-1};
    int  count_at{-1};

    float confidence{0.0f};
    std::vector<std::string> evidence;

    bool Valid() const { return names_array >= 0; }
};

UEnumLayout DeriveEnumLayout(core::IMemorySource& memory,
                             const ObjectArrayInfo& array,
                             const NamePoolInfo& pool,
                             const UObjectLayout& object_layout);

// Entry names come back fully qualified as UE stores them ("EMovementMode::MOVE_Walking").
std::vector<std::pair<std::string, std::int64_t>> GetEnumValues(
    core::IMemorySource& memory, const UEnumLayout& layout, const NamePoolInfo& pool,
    core::Address enum_object);

} // namespace zircon::engine
