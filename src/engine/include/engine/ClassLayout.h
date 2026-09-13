#pragma once

#include "core/MemorySource.h"
#include "engine/NamePool.h"
#include "engine/ObjectArray.h"
#include "engine/ObjectLayout.h"
#include "engine/StructLayout.h"

#include <string>
#include <vector>

namespace zircon::engine {

// Offsets within UClass, past the UStruct part.
//
// Only one member is derived here, but it is the one that turns a class from a name with
// offsets into something with *values*: the class default object. Every property a class
// declares has a default, and the CDO is where the engine keeps it. Without it a class
// selected in the browser has nothing to show, because a class is not an instance.
struct UClassLayout {
    int class_default_object{-1};   // UObject* ClassDefaultObject

    // TArray<FImplementedInterface> Interfaces. The element is
    // { UClass* Class; int32 PointerOffset; bool bImplementedByK2; }, so the stride is
    // derived, never assumed: a licensee build is free to pad it differently.
    int interfaces{-1};
    int interface_stride{0};

    float confidence{0.0f};
    std::vector<std::string> evidence;

    bool Valid() const { return class_default_object >= 0; }
    bool HasInterfaces() const { return interfaces >= 0 && interface_stride > 0; }
};

// The CDO is identified by a property no other pointer field in UClass has: the object it
// points at reports *this very class* as its class. That is a closed loop nothing else
// closes — SuperStruct points at a different class, ClassWithin at UObject, and the
// various linked lists at properties and functions.
//
// The loop alone is still only an internal-consistency test, and this project has been
// burned six times by fields that satisfy one of those perfectly while being wrong. So it
// is paired with two independent facts with known-correct values: the target must be a
// live entry of the object array (its slot index round-trips), and the engine names every
// CDO "Default__<ClassName>". A field cannot fake all three.
UClassLayout DeriveClassLayout(core::IMemorySource& memory,
                               const ObjectArrayInfo& array,
                               const NamePoolInfo& pool,
                               const UObjectLayout& object_layout,
                               const UStructLayout& struct_layout);

// The class default object of a class, or null when the class has none (the abstract and
// the not-yet-constructed) or the layout was not derived.
core::Address GetClassDefaultObject(core::IMemorySource& memory, const UClassLayout& layout,
                                    core::Address klass);

// The interfaces a class declares, as object addresses. Empty when the class implements
// none, or when the layout could not be derived.
std::vector<core::Address> GetClassInterfaces(core::IMemorySource& memory,
                                              const ObjectArrayInfo& array,
                                              const UClassLayout& layout,
                                              core::Address klass);

} // namespace zircon::engine
