#pragma once

// Writes a new value into a live property.
//
// The inverse of ValueReader, and deliberately narrower. A value is only written when the
// text parses cleanly, the property is a kind whose in-memory representation is fully
// understood, and the write covers exactly the bytes the property owns. Anything else is
// refused with a reason.
//
// The refusals matter more than the successes. A container or a struct has internal state
// (allocators, element counts, cached hashes) that a naive byte write corrupts without any
// immediate symptom, so those kinds report that they are unsupported instead of writing
// something that looks like it worked.

#include "core/MemorySource.h"
#include "engine/TypeResolver.h"

#include <string>
#include <string_view>
#include <vector>

namespace zircon::engine {

struct WriteResult {
    bool        ok{false};
    std::string error;        // set when ok is false
    std::string written;      // how the value reads back, for the log

    explicit operator bool() const { return ok; }
};

// Whether this property kind can be written at all. Lets a UI grey out a row instead of
// offering an edit that will be refused.
bool IsPropertyWritable(const ResolveContext& context, core::Address field);

// Parses `text` per the property's type and writes it. `text` accepts what the reader
// prints: decimal or 0x hex for integers, true/false for bools, and either an enumerator
// name or its number for enums.
//
// Requires the memory source to have writes enabled; it does not enable them itself.
WriteResult WritePropertyValue(const ResolveContext& context, core::Address object,
                               core::Address field, std::string_view text);

// The enumerators of an enum property, for a UI that offers a list instead of a text box.
// Empty when the property is not an enum or the enum layout was not derived.
std::vector<std::string> EnumeratorNames(const ResolveContext& context, core::Address field);

} // namespace zircon::engine
