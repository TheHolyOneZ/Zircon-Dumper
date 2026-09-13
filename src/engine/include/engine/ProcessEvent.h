#pragma once

// Finds UObject::ProcessEvent, the entry point every reflected call goes through.
//
// It is a virtual, and its vtable index is the one thing about calling that the reflection
// data does not record. Nothing in the object graph names a virtual slot, so it cannot be
// read the way every other offset here is.
//
// What it can be is verified. UE ships a pure function whose answer is known ahead of
// time: KismetMathLibrary::Add_IntInt. Call a candidate slot with 2 and 3, and the slot
// that returns 5 is ProcessEvent. That is the same shape as every other derivation in this
// project, a candidate plus an independent anchor, except the check costs a call instead
// of a read.
//
// The cost is real and worth stating plainly. A wrong slot is an arbitrary virtual invoked
// with two pointers it was not expecting, and some of those crash the game. Calls are
// guarded, candidates that do not point into executable memory are skipped, and the first
// slots are left alone because a destructor lives there. It still cannot be made safe, so
// nothing calls this unless asked.
//
// Only works in-process: reading memory is enough for everything else this tool does, but
// calling a function requires being inside the address space that owns it.

#include "engine/ClassLayout.h"
#include "engine/TypeResolver.h"

#include <string>
#include <vector>

namespace zircon::engine {

struct ProcessEventInfo {
    int vtable_index{-1};

    float confidence{0.0f};
    std::vector<std::string> evidence;

    bool Valid() const { return vtable_index >= 0; }
};

// Requires `context.memory->Caps().can_call`. An invalid result means the probe
// function was missing or no slot answered correctly. It never guesses one.
//
// `classes` is passed separately because a class default object is the only thing this
// needs from it, and ResolveContext is threaded through the whole engine layer for callers
// that have no interest in classes at all.
ProcessEventInfo DeriveProcessEvent(const ResolveContext& context,
                                    const UClassLayout& classes);

} // namespace zircon::engine
