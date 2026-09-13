#pragma once

#include "core/MemorySource.h"
#include "engine/EngineProfile.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace zircon::engine {

// The global UObject table. Everything the reflection walker does starts here.
//
// Located by validating semantics, not by matching bytes, which is why this
// survives engine forks: we do not care what the surrounding code looks like, only
// that the structure behaves like an object array when we follow it.
struct ObjectArrayInfo {
    core::Address inner{};        // the FChunkedFixedUObjectArray itself
    core::Address gobjects{};     // the enclosing FUObjectArray, what people call GObjects

    bool          chunked{true};
    std::int32_t  num_elements{};
    std::int32_t  max_elements{};
    std::int32_t  num_chunks{};
    std::int32_t  max_chunks{};
    std::uint32_t elements_per_chunk{};
    std::uint32_t item_size{};    // sizeof(FUObjectItem)

    // Where the UObject* sits inside an FUObjectItem.
    //
    // It was the first member through every engine up to 5.6, so reading the item as a
    // pointer worked and nothing needed this. UE 5.7 puts the flags first and the pointer
    // at +8, which does not fail gracefully: every slot reads as a flags word, no slot
    // stores its own index, and the array is simply never found — the search reports
    // nothing and gives no hint that one offset is the whole problem.
    int           item_object_offset{0};

    // Derived, not assumed: the offset of UObject::InternalIndex. Falls out of the same
    // check that proves this is the object array, since an object's stored index must
    // equal the slot it was found in.
    int index_offset{-1};

    float confidence{0.0f};
    std::vector<std::string> evidence;

    bool Valid() const { return !core::IsNull(inner) && num_elements > 0; }
};

// Scans for the object array. Requires a source with live_objects: a PE on disk has an
// empty .data, so there is nothing to find.
std::optional<ObjectArrayInfo> FindObjectArray(core::IMemorySource& memory,
                                               const EngineProfile& profile);

// The UObject* stored in slot `index`, or a null Address when out of range or unreadable.
core::Address ObjectAt(core::IMemorySource& memory, const ObjectArrayInfo& info,
                       std::int32_t index);

} // namespace zircon::engine
