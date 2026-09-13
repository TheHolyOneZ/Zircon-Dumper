#pragma once

#include "core/MemorySource.h"
#include "engine/EngineProfile.h"
#include "engine/ClassLayout.h"
#include "engine/EnumLayout.h"
#include "engine/FunctionLayout.h"
#include "engine/NamePool.h"
#include "engine/ObjectArray.h"
#include "engine/ObjectLayout.h"
#include "engine/PropertyLayout.h"
#include "engine/StructLayout.h"
#include "engine/Kismet.h"
#include "engine/TypeResolver.h"
#include "ir/Model.h"

#include <string>

namespace zircon::engine {

// Everything P1 and P2 derive about one target, gathered so the builder can be called
// with a single argument instead of nine.
struct Reflection {
    core::IMemorySource* memory{};
    EngineProfile   profile;
    ObjectArrayInfo array;
    NamePoolInfo    pool;
    UObjectLayout   object_layout;
    UStructLayout   struct_layout;
    UClassLayout    class_layout;
    FPropertyLayout property_layout;
    SubclassLayout  subclass_layout;
    UFunctionLayout function_layout;
    UEnumLayout     enum_layout;
    ScriptLayout    script_layout;

    ResolveContext Context() const;
    bool Valid() const;
};

// Derives everything needed to walk a target. Returns a Reflection whose Valid() says
// whether the walk can proceed.
Reflection Reflect(core::IMemorySource& memory);

struct BuildOptions {
    bool include_names{false};       // embed the whole FName pool

    // Decompile Kismet bytecode into each function. Opt-in because it roughly doubles a
    // large dump and most consumers do not need it.
    bool include_script{false};
    std::string package_filter;      // substring; empty means every package

    // Read every property's value out of its class's default object. Opt-in because it
    // costs several memory reads per property across the whole dump, and because it needs
    // a live target: a static PE has no constructed objects to read from.
    bool include_defaults{false};
};

ir::Dump BuildDump(const Reflection& reflection, const BuildOptions& options = {});

} // namespace zircon::engine
