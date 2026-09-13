#pragma once

#include "core/MemorySource.h"
#include "engine/NamePool.h"
#include "engine/ObjectArray.h"

#include <string>
#include <vector>

namespace zircon::engine {

// Offsets within UObject. Every one of these is derived from observed behaviour rather
// than looked up in a per-version table, which is the whole point: a licensee build with
// an extra member shifts these, and a table would be wrong without admitting it.
struct UObjectLayout {
    int index_offset{-1};    // InternalIndex, already derived while finding the array
    int class_offset{-1};    // ClassPrivate
    int name_offset{-1};     // NamePrivate (FName: two int32)
    int outer_offset{-1};    // OuterPrivate

    // The object whose class is itself. In UE that is UClass, and it is the fixed point
    // that separates ClassPrivate from OuterPrivate.
    core::Address uclass_object{};

    float confidence{0.0f};
    std::vector<std::string> evidence;

    bool Valid() const {
        return class_offset >= 0 && name_offset >= 0 && outer_offset >= 0;
    }
};

UObjectLayout DeriveObjectLayout(core::IMemorySource& memory,
                                 const ObjectArrayInfo& array,
                                 const NamePoolInfo& pool);

// --- object accessors ---------------------------------------------------------------

std::string GetObjectName(core::IMemorySource& memory, const UObjectLayout& layout,
                          const NamePoolInfo& pool, core::Address object);

// The raw FName comparison index of an object. Used to build a stable identity for the
// handful of objects whose name does not resolve: an empty name is not an identity, and
// several of them in one type collapse into each other.
std::uint32_t GetObjectNameId(core::IMemorySource& memory, const UObjectLayout& layout,
                              core::Address object);

core::Address GetObjectClass(core::IMemorySource& memory, const UObjectLayout& layout,
                             core::Address object);

core::Address GetObjectOuter(core::IMemorySource& memory, const UObjectLayout& layout,
                             core::Address object);

// "/Script/Engine.Actor" — the outer chain resolved outermost-first. Package names
// already begin with a slash, so only inner links get a separator.
std::string GetObjectPathName(core::IMemorySource& memory, const UObjectLayout& layout,
                              const NamePoolInfo& pool, core::Address object);

// "Class /Script/Engine.Actor" — the path name prefixed with the class name.
std::string GetObjectFullName(core::IMemorySource& memory, const UObjectLayout& layout,
                              const NamePoolInfo& pool, core::Address object);

} // namespace zircon::engine
