#pragma once

// Reading a Unity game's type system off disk, without running it.
//
// Knows the type set exactly, and repeatably -- names, namespaces, tokens, assemblies, which
// fields and methods a type declares. The live walk can't promise repeatable: its class cache
// grows while the game runs.
//
// Doesn't know what anything *is*. Field types, field offsets and method addresses live in the
// binary, not here, so they come back unresolved with nothing invented. A partial answer that
// says which half is missing.
//
// Reaches targets the walk can't: an APK, a console build, anti-cheat, a game that crashes
// the moment the walk touches it.

#include "il2cpp/Metadata.h"
#include "ir/Model.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace zircon::il2cpp {

struct StaticStats {
    std::size_t images{0};
    std::size_t types{0};
    std::size_t fields{0};
    std::size_t methods{0};

    // Types the image ranges never reach. A file where this is not zero is one where the
    // image table and the type table disagree, which is worth saying out loud.
    std::size_t orphan_types{0};
};

// Turns a solved layout into the same ir::Dump the injected walk produces, so every emitter,
// the linter and the diff work on it unchanged.
ir::Dump ReadStaticDump(std::span<const std::uint8_t> file, const MetadataLayout& layout,
                        StaticStats& stats);

struct MergeStats {
    std::size_t in_both{0};          // types the runtime and the metadata both described
    std::size_t live_only{0};        // instantiations, which exist only once something runs
    std::size_t static_only{0};      // declared but never touched, so never in the cache
    std::size_t conflicts{0};
};

// Dual mode: one dump out of both readings.
//
// The metadata is the spine, because its type set doesn't move between two reads. The runtime
// wins on anything that only exists once something has run -- offsets, method addresses,
// concrete generics. A type only one side saw is kept and marked with which side saw it;
// dropping either would lose something real.
//
// Every disagreement goes in header.conflicts in full. On a packed build the disagreement is
// the finding, and a merge that quietly picked a side would destroy it.
ir::Dump MergeDumps(const ir::Dump& live, const ir::Dump& from_metadata, MergeStats& stats);

} // namespace zircon::il2cpp
