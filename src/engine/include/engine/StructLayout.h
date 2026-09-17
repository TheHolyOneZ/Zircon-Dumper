#pragma once

#include "core/MemorySource.h"
#include "engine/NamePool.h"
#include "engine/ObjectArray.h"
#include "engine/ObjectLayout.h"

#include <cstdint>
#include <string>
#include <vector>

namespace zircon::engine {

// Offsets within UStruct, the base of UClass, UScriptStruct and UFunction. Derived, like
// everything else, from invariants that hold because of what the engine *is* rather than
// which version it happens to be.
struct UStructLayout {
    int super_struct{-1};      // UStruct* SuperStruct
    int children{-1};          // UField*  Children          (UObjects: functions, enums)
    int child_properties{-1};  // FField*  ChildProperties    (FProperties, 4.25+)
    int properties_size{-1};   // int32    PropertiesSize
    int min_alignment{-1};     // int32    MinAlignment
    bool min_alignment_is_u16{false};   // some builds store it narrower than int32

    // The object for /Script/CoreUObject.Object, found while deriving. It is the root of
    // every class chain and a useful anchor for later passes.
    core::Address object_class{};

    // What /Script/CoreUObject.Object reports as its own size, i.e. sizeof(UObject) in the
    // target. Stock builds put OuterPrivate last, so this is object_layout.outer_offset + 8;
    // a fork that appends its own fields to UObject makes it larger, and then it is the only
    // honest source for that number. -1 until PropertiesSize is derived.
    std::int32_t object_size{-1};

    // Set when object_size came out larger than a stock UObject, i.e. the target extends
    // UObject itself. Worth surfacing: it says the build is a fork at the deepest level, not
    // merely a renamed executable.
    bool extends_uobject{false};

    float confidence{0.0f};
    std::vector<std::string> evidence;

    bool Valid() const { return super_struct >= 0 && properties_size >= 0; }
};

UStructLayout DeriveStructLayout(core::IMemorySource& memory,
                                 const ObjectArrayInfo& array,
                                 const NamePoolInfo& pool,
                                 const UObjectLayout& object_layout);

// --- helpers used by the walker and by later phases ---------------------------------

// The class name of an object, e.g. "Class", "Function", "ScriptStruct", "Package".
std::string GetClassName(core::IMemorySource& memory, const UObjectLayout& layout,
                         const NamePoolInfo& pool, core::Address object);

// What an object *is*, determined by walking its meta-class chain instead of by
// comparing its class name.
//
// Comparing names does not work: a Blueprint class has meta-class
// BlueprintGeneratedClass, a widget WidgetBlueprintGeneratedClass, an anim graph
// AnimBlueprintGeneratedClass. Testing for the literal name "Class" quietly excludes
// every Blueprint type in the game — which is exactly what happened here, and it went
// unnoticed until the bytecode decompiler found functions the dump did not contain.
enum class ObjectKind { Other, Class, ScriptStruct, Enum, Function, Package };

ObjectKind ClassifyObject(core::IMemorySource& memory, const UObjectLayout& object_layout,
                          const UStructLayout& struct_layout, const NamePoolInfo& pool,
                          core::Address object);

core::Address GetSuperStruct(core::IMemorySource& memory, const UStructLayout& layout,
                             core::Address structure);

std::int32_t GetMinAlignment(core::IMemorySource& memory, const UStructLayout& layout,
                             core::Address structure);

std::int32_t GetPropertiesSize(core::IMemorySource& memory, const UStructLayout& layout,
                               core::Address structure);

// First object in the array whose full path name matches exactly. Linear, so it is for
// anchoring and diagnostics rather than for use inside a walk.
core::Address FindObjectByPath(core::IMemorySource& memory, const ObjectArrayInfo& array,
                               const UObjectLayout& layout, const NamePoolInfo& pool,
                               std::string_view path);

} // namespace zircon::engine
