#pragma once

// Reads the live value of a property out of a running object.
//
// What separates a browser from a dump. The dump says RootComponent sits at 0x150 and is
// a SceneComponent*; this says what is actually sitting there right now.
//
// Values are formatted for display, never parsed back, so the priority is being honest
// about what could not be read over producing something printable at all costs.

#include "core/MemorySource.h"
#include "engine/TypeResolver.h"

#include <string>

namespace zircon::engine {

struct ValueFormat {
    int  max_array_items{6};   // a TArray of 40000 is not useful inline
    int  max_depth{2};         // struct-in-struct-in-struct stops being readable
    int  max_string{160};
    bool hex_integers{false};
};

// Formats the value of `field` within `object`. Returns a placeholder such as
// "<unreadable>" rather than a plausible number when the memory cannot be read — a
// browser that shows 0 for a failed read is worse than one that shows nothing.
std::string ReadPropertyValue(const ResolveContext& context, core::Address object,
                              core::Address field, const ValueFormat& format = {});

// FString is TArray<wchar_t>; read here because several callers need it directly.
std::string ReadFString(core::IMemorySource& memory, core::Address address,
                        int max_chars = 1024);

} // namespace zircon::engine
