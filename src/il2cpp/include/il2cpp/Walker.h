#pragma once

// Turns a live IL2CPP runtime into the same ir::Dump the Unreal path produces.
//
// Sharing the IR is the reason this lives inside Zircon at all: every emitter, the linter,
// diff and the browser already take a Dump, so Unity gets all of them for free. The IR grew
// what it was missing -- namespace, unboxed offsets, metadata token, a runtime field.
//
// The walk is the ordinary one: domain -> assemblies -> images -> classes -> members. The
// interesting parts are where it refuses -- open generics, consts a build won't hand over,
// bodies when MethodInfo couldn't be placed.

#include "il2cpp/Bridge.h"
#include "ir/Model.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace zircon::il2cpp {

struct WalkOptions {
    // Substring match on the full type name. Empty keeps everything, which is what makes a
    // dump diffable against the next build.
    std::string filter;

    // Sweep the class cache for generic instantiations. List`1 is in the metadata with no
    // real offsets; List<int> is built at runtime and has them. Off = smaller and less true.
    bool include_inflated{true};

    // Ask the runtime for const values. Whether it answers depends on the build; the enums
    // record which they got.
    bool resolve_enum_values{true};

    // Called with what the walk is about to touch, before it touches it. The walk doesn't
    // care where that goes -- the payload puts it in a mapped page so a crash names the
    // type. Empty is fine and costs one branch per class.
    std::function<void(std::string_view)> breadcrumb;

    // Types to walk past without reading. For a build with a class that faults, this is how
    // you get the rest of the dump. Exact path match, not a substring: skipping more than
    // you meant to is a dump that quietly lost things.
    std::vector<std::string> skip;
};

struct WalkStats {
    std::size_t assemblies{0};
    std::size_t images{0};
    std::size_t classes{0};
    std::size_t enums{0};
    std::size_t fields{0};
    std::size_t methods{0};
    std::size_t accessors{0};

    std::size_t inflated{0};             // generic instantiations found in the class cache

    // Instantiations whose name was already taken -- IEnumerable<T> built over another
    // generic's T, many times over, nothing in the name to tell them apart.
    std::size_t indistinguishable{0};
    std::size_t open_generics{0};        // definitions whose offsets cannot be answered
    std::size_t unresolved_offsets{0};
    std::size_t shared_bodies{0};        // methods sharing an address with another method
    std::size_t bodies{0};               // methods that got one at all
    std::size_t enums_without_values{0};
    std::size_t skipped{0};              // types the caller asked us to walk past

    // Types whose base the runtime reports as larger than they are. Their inherited
    // size is left out rather than written down as a contradiction.
    std::size_t contradictory_bases{0};
};

ir::Dump Walk(IBridge& bridge, const WalkOptions& options, WalkStats& stats);

} // namespace zircon::il2cpp
