#pragma once

#include "core/MemorySource.h"
#include "engine/NamePool.h"
#include "engine/ObjectArray.h"
#include "engine/ObjectLayout.h"
#include "engine/EngineProfile.h"
#include "engine/PropertyLayout.h"
#include "engine/StructLayout.h"

#include <string>
#include <vector>

namespace zircon::engine {

// Where each FProperty subclass keeps the member that names its inner type.
//
// These are derived separately, not from one shared sizeof(FProperty), because
// the subclasses do not share a base depth: FObjectProperty descends through
// FObjectPropertyBase, while FArrayProperty derives from FProperty directly, so their
// first members land at different offsets. Measured on UE 5.6, PropertyClass sits at
// +0x70 while Inner sits at +0x78. Assume a single offset and every
// container element to nothing.
struct SubclassLayout {
    int object_property_class{-1};  // FObjectPropertyBase::PropertyClass  (UClass*)
    int class_meta_class{-1};       // FClassProperty::MetaClass           (UClass*)
    int struct_struct{-1};          // FStructProperty::Struct             (UScriptStruct*)
    int array_inner{-1};            // FArrayProperty::Inner               (FProperty*)
    int set_element{-1};            // FSetProperty::ElementProp           (FProperty*)
    int map_key{-1};                // FMapProperty::KeyProp               (FProperty*)
    int map_value{-1};              // FMapProperty::ValueProp             (FProperty*)
    int enum_underlying{-1};        // FEnumProperty::UnderlyingProp       (FProperty*)
    int enum_enum{-1};              // FEnumProperty::Enum                 (UEnum*)
    int byte_enum{-1};              // FByteProperty::Enum                 (UEnum*)
    int interface_class{-1};        // FInterfaceProperty::InterfaceClass  (UClass*)
    int delegate_signature{-1};     // FDelegateProperty::SignatureFunction(UFunction*)
    int bool_masks{-1};             // FBoolProperty: FieldSize/ByteOffset/ByteMask/FieldMask

    float confidence{0.0f};
    std::vector<std::string> evidence;

    // Object and struct slots are the ones everything else leans on; a layout without
    // them cannot resolve any reference at all.
    bool Valid() const { return object_property_class >= 0 && struct_struct >= 0; }
};

SubclassLayout DeriveSubclassLayout(core::IMemorySource& memory,
                                    const ObjectArrayInfo& array,
                                    const NamePoolInfo& pool,
                                    const UObjectLayout& object_layout,
                                    const UStructLayout& struct_layout,
                                    const FPropertyLayout& property_layout);

// A property's type, resolved as far as the engine's own data allows. Mirrors ir::TypeRef
// but stays engine-local so the engine layer does not need the IR to do its job; the
// conversion happens at the boundary.
struct ResolvedType {
    std::string raw;                    // engine property class, e.g. "ObjectProperty"
    std::string referenced;             // path of the referenced class/struct/enum
    std::vector<ResolvedType> params;   // element / key / value
    std::int32_t size{0};

    // The same referenced object, as the address it was read from. The path is what goes
    // into the IR; this is for callers that then need the object itself — reading a live
    // enum value, say. Looking the path back up would be a linear scan of every object in
    // the target, per property, when the pointer was already in hand.
    core::Address referenced_object{};
};

struct BitfieldInfo {
    bool         is_bitfield{false};   // false for a real bool, true when packed
    std::uint8_t byte_mask{0};
    std::uint8_t field_mask{0};
    std::int32_t bit_index{-1};
};

// Context bundle: resolution needs nearly every layout, and threading seven parameters
// through a recursive function makes the recursion unreadable.
struct ResolveContext {
    core::IMemorySource*   memory{};
    const ObjectArrayInfo* array{};
    const NamePoolInfo*    pool{};
    const UObjectLayout*   object_layout{};
    const UStructLayout*   struct_layout{};
    const FPropertyLayout* property_layout{};
    const SubclassLayout*  subclass_layout{};

    // Optional. Only the bytecode decompiler needs it, and only for the handful of
    // opcodes whose operand width changed between UE4 and UE5.
    const EngineProfile*   profile{};

    // Optional. Lets a live value show as MOVE_Walking instead of 1.
    const struct UEnumLayout* enum_layout{};

    // Optional, and only the bytecode decompiler reads it: it carries the measured width
    // of the engine's vector constants.
    const struct ScriptLayout* script_layout{};
};

ResolvedType  ResolveType(const ResolveContext& context, core::Address field);
BitfieldInfo  ResolveBitfield(const ResolveContext& context, core::Address field);

// "TArray<FVector>", "TMap<FName, int32>", "AActor*" — a readable rendering, used by the
// CLI and by the docs emitter. The C++ SDK emitter does its own rendering because it has
// to care about forward declarations.
std::string DescribeType(const ResolvedType& type);

} // namespace zircon::engine
