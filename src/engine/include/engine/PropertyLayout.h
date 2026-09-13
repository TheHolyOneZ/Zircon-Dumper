#pragma once

#include "core/MemorySource.h"
#include "engine/NamePool.h"
#include "engine/ObjectArray.h"
#include "engine/ObjectLayout.h"
#include "engine/StructLayout.h"

#include <cstdint>
#include <string>
#include <vector>

namespace zircon::engine {

// Where a property keeps its parts, in whichever of Unreal's two property systems this
// build uses.
//
// 4.25 and later: properties are FFields, which are not UObjects — they live outside the
// object array, which is exactly how ChildProperties was told apart from Children in the
// first place.
//
// 4.24 and earlier: properties are UProperty, which *is* a UObject, reached through
// UStruct::Children alongside functions and enums. Every field below means the same thing
// in both worlds and simply sits somewhere else, so one set of accessors serves both and
// nothing downstream has to know which era it is walking.
struct FPropertyLayout {
    // True when properties are UObjects in Children instead of FFields in
    // ChildProperties. Derived from the absence of ChildProperties, which is the structural
    // fact that distinguishes the two systems — not from a version number.
    bool uproperty{false};

    // FField / UProperty
    int class_private{-1};    // FFieldClass* or UClass*
    int next{-1};             // FField::Next or UField::Next
    int name{-1};             // FName NamePrivate

    // Where the type's own name sits inside whatever class_private points at. An
    // FFieldClass begins with its FName, so zero; a UClass keeps it where every UObject
    // does.
    int class_name_offset{0};

    // Raw addresses of every UClass that is a property class, sorted. Only used in the
    // UProperty era, where Children mixes properties with functions and enums and each
    // entry has to be told apart. Kept as a set so the test is a binary search rather than
    // a super-chain walk per entry per struct.
    std::vector<std::uint64_t> property_classes;

    // FProperty
    int array_dim{-1};        // int32
    int element_size{-1};     // int32  (bytes per element)
    int property_flags{-1};   // uint64 EPropertyFlags
    int offset_internal{-1};  // int32  offset of the property within its owning struct

    float confidence{0.0f};
    std::vector<std::string> evidence;

    bool Valid() const {
        return class_private >= 0 && next >= 0 && name >= 0 && offset_internal >= 0 &&
               element_size >= 0;
    }
};

FPropertyLayout DerivePropertyLayout(core::IMemorySource& memory,
                                     const ObjectArrayInfo& array,
                                     const NamePoolInfo& pool,
                                     const UObjectLayout& object_layout,
                                     const UStructLayout& struct_layout);

// --- accessors ----------------------------------------------------------------------

std::string GetFieldName(core::IMemorySource& memory, const FPropertyLayout& layout,
                         const NamePoolInfo& pool, core::Address field);

// The reflected type of a property: "ObjectProperty", "IntProperty", "ArrayProperty"...
std::string GetPropertyTypeName(core::IMemorySource& memory, const FPropertyLayout& layout,
                                const NamePoolInfo& pool, core::Address field);

core::Address GetNextField(core::IMemorySource& memory, const FPropertyLayout& layout,
                           core::Address field);

std::int32_t  GetPropertyOffset(core::IMemorySource& memory, const FPropertyLayout& layout,
                                core::Address field);
std::int32_t  GetElementSize(core::IMemorySource& memory, const FPropertyLayout& layout,
                             core::Address field);
std::int32_t  GetArrayDim(core::IMemorySource& memory, const FPropertyLayout& layout,
                          core::Address field);
std::uint64_t GetPropertyFlags(core::IMemorySource& memory, const FPropertyLayout& layout,
                               core::Address field);

// Every property declared directly by `structure`, in declaration order. Inherited
// properties are deliberately excluded: they belong to the base, and emitting them again
// would double-count every field in a derived class.
std::vector<core::Address> GetChildProperties(core::IMemorySource& memory,
                                              const UStructLayout& struct_layout,
                                              const FPropertyLayout& property_layout,
                                              core::Address structure);

} // namespace zircon::engine
