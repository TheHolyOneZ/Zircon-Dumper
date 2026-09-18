#pragma once

// Where a compiled method body lives. Derived, not looked up.
//
// The API gives us name, class, return type, params, flags, token -- but there's no
// il2cpp_method_get_pointer, and the body is the thing people actually want. So it has to
// come out of MethodInfo, whose layout moves: 2021.2 added a second code pointer ahead of
// the invoker, and fields keep getting appended.
//
// No struct-per-version table here, same as src/engine/. We derive the head of MethodInfo
// from the process, anchored on values the API hands us:
//
//   name, klass, return_type   exact pointer matches. the API returns the very pointer the
//                              struct holds, so the slot that holds it isn't a guess
//   token                      exact 32-bit match, corroborates the rest
//   the code pointers          everything below `name`, landing on function entry points
//
// Only the last needs judgement: two or three code pointers, one body. Told apart by what
// an invoker is -- one thunk shared by every method of the same shape. So in a group of
// methods with the same arg count the invoker slot is constant and the body slot varies.
// Positive constraint both ways. See CONTRIBUTING.md on why "doesn't look wrong" has been
// wrong every single time.

#include "core/MemorySource.h"
#include "core/PeImage.h"
#include "core/Types.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace zircon::il2cpp {

// One method as the API describes it. Raw addresses, because the derivation works by
// recognising these exact values inside the MethodInfo block.
struct MethodProbe {
    core::Address method{};
    core::Address name{};          // il2cpp_method_get_name
    core::Address klass{};         // il2cpp_method_get_class
    core::Address return_type{};   // il2cpp_method_get_return_type
    std::uint32_t param_count{0};
    std::uint32_t token{0};
    bool          is_instance{true};
};

struct MethodInfoLayout {
    std::int32_t body{-1};         // Il2CppMethodPointer -- the compiled code
    std::int32_t invoker{-1};      // InvokerMethod -- shared across one signature shape
    std::int32_t name{-1};
    std::int32_t klass{-1};
    std::int32_t return_type{-1};
    std::int32_t token{-1};

    // Second code pointer between body and invoker. Unity added it in 2021.2 for value-type
    // adjustor thunks; -1 on older builds. Recorded so "three slots, took the first" is
    // something a reader can check.
    std::int32_t virtual_body{-1};

    bool Valid() const { return body >= 0; }

    std::vector<std::string> evidence;
    std::string              refusal;   // why not, when Valid() is false
};

// Derives the layout from a sample of methods. Spread the sample across classes -- from one
// class every klass pointer is identical and any constant slot scores as well as the real
// one.
//
// `executable`: the module's code sections. Decides whether a slot holds code at all; a
// pointer into the metadata sits in .rdata and fails. `function_starts`: the exception
// directory, which confirms those are function starts and not the middle of .text.
// Corroboration, not a gate -- leaf functions have no unwind data and invoker thunks are
// mostly leaves. Code slots score 20-80% here on real builds, data slots score zero.
MethodInfoLayout DeriveMethodInfoLayout(core::IMemorySource& memory,
                                        std::span<const MethodProbe> sample,
                                        core::Address module_base,
                                        std::uint64_t module_size,
                                        std::span<const std::uint32_t> function_starts,
                                        std::span<const core::RegionInfo> executable);

} // namespace zircon::il2cpp
