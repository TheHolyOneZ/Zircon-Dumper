#include "engine/ProcessEvent.h"

#include "core/Log.h"
#include "engine/ClassLayout.h"
#include "engine/PropertyLayout.h"
#include "engine/StructLayout.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>
#include <format>
#include <vector>

namespace zircon::engine {
namespace {

using core::Address;
using core::IsNull;
using core::Raw;

using ProcessEventFn = void(__fastcall*)(void* self, void* function, void* params);

// The call, alone in a function with nothing that needs unwinding, because __try cannot
// coexist with objects that have destructors.
//
// A wrong slot is an arbitrary virtual handed two pointers it did not ask for. Access
// violations are caught here; a slot that corrupts something quietly is not, which is why
// the whole routine is opt-in.
bool TryCall(ProcessEventFn candidate, void* self, void* function, void* params) {
    __try {
        candidate(self, function, params);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Vtable slots that hold something the CPU could execute. Cheap, and it removes most of a
// vtable's tail before anything is called.
bool PointsIntoCode(core::IMemorySource& memory, Address target) {
    if (IsNull(target)) return false;

    const auto* region = memory.RegionContaining(target);
    return region != nullptr && core::HasFlag(region->protect, core::RegionProtect::Execute);
}

struct Probe {
    Address      function{};      // the UFunction to invoke
    Address      self{};          // the object to invoke it on
    std::int32_t params_size{0};
    std::int32_t a_offset{-1};
    std::int32_t b_offset{-1};
    std::int32_t return_offset{-1};
    std::string  path;
};

// A function whose answer is known without running it. Its parameter offsets come from
// the same reflection data as everything else, so no layout is assumed here.
bool BuildProbe(const ResolveContext& context, const UClassLayout& classes,
                Probe& probe) {
    auto& memory = *context.memory;

    // Any of these will do. They are pure, take two ints and return one, and have been in
    // KismetMathLibrary since UE4.
    for (const char* path : {"/Script/Engine.KismetMathLibrary.Add_IntInt",
                             "/Script/Engine.KismetMathLibrary.Multiply_IntInt",
                             "/Script/Engine.KismetMathLibrary.Subtract_IntInt"}) {

        const auto function = FindObjectByPath(memory, *context.array, *context.object_layout,
                                               *context.pool, path);
        if (IsNull(function)) continue;

        // Static functions dispatch through the class default object, which is what UE
        // does for a Blueprint function library.
        const auto owner = GetObjectOuter(memory, *context.object_layout, function);
        if (IsNull(owner)) continue;

        const auto cdo = GetClassDefaultObject(memory, classes, owner);
        if (IsNull(cdo)) continue;

        probe = {};
        probe.function    = function;
        probe.self        = cdo;
        probe.path        = path;
        probe.params_size = GetPropertiesSize(memory, *context.struct_layout, function);

        for (const auto field : GetChildProperties(memory, *context.struct_layout,
                                                   *context.property_layout, function)) {
            const std::string name = GetFieldName(memory, *context.property_layout,
                                                  *context.pool, field);
            const std::int32_t offset =
                GetPropertyOffset(memory, *context.property_layout, field);

            if      (name == "A")           probe.a_offset = offset;
            else if (name == "B")           probe.b_offset = offset;
            else if (name == "ReturnValue") probe.return_offset = offset;
        }

        if (probe.params_size > 0 && probe.a_offset >= 0 && probe.b_offset >= 0 &&
            probe.return_offset >= 0)
            return true;
    }
    return false;
}

// What the probe should produce, given the operation its name describes.
std::int32_t Expected(const std::string& path, std::int32_t a, std::int32_t b) {
    if (path.find("Multiply_IntInt") != std::string::npos) return a * b;
    if (path.find("Subtract_IntInt") != std::string::npos) return a - b;
    return a + b;
}

// One call through one slot, answering whether it produced the right number.
bool SlotAnswers(const Probe& probe, ProcessEventFn candidate, std::int32_t a,
                 std::int32_t b) {
    std::vector<std::uint8_t> params(static_cast<std::size_t>(probe.params_size), 0);
    std::memcpy(params.data() + probe.a_offset, &a, sizeof(a));
    std::memcpy(params.data() + probe.b_offset, &b, sizeof(b));

    if (!TryCall(candidate, reinterpret_cast<void*>(Raw(probe.self)),
                 reinterpret_cast<void*>(Raw(probe.function)), params.data()))
        return false;

    std::int32_t result = 0;
    std::memcpy(&result, params.data() + probe.return_offset, sizeof(result));
    return result == Expected(probe.path, a, b);
}

} // namespace

ProcessEventInfo DeriveProcessEvent(const ResolveContext& context,
                                    const UClassLayout& classes) {
    ProcessEventInfo info;

    if (!context.memory || !context.array || !context.object_layout ||
        !context.struct_layout || !context.property_layout || !classes.Valid()) {
        core::LogWarn("ProcessEvent: the layout is incomplete");
        return info;
    }

    auto& memory = *context.memory;
    if (!memory.Caps().can_call) {
        core::LogWarn("ProcessEvent can only be found from inside the process");
        return info;
    }

    Probe probe;
    if (!BuildProbe(context, classes, probe)) {
        core::LogWarn("ProcessEvent: no probe function with a known answer was found");
        return info;
    }

    const auto vtable = core::ReadOr<Address>(memory, probe.self);
    if (IsNull(vtable)) {
        core::LogWarn("ProcessEvent: {} has no vtable", probe.path);
        return info;
    }

    core::LogInfo("ProcessEvent: probing with {} on the CDO at {:#x}", probe.path,
                  Raw(probe.self));

    // The first slots hold the destructor. Calling one on a class default object is not
    // something to recover from, so they are never tried.
    constexpr int kFirstSlot = 2;
    constexpr int kLastSlot  = 160;

    for (int slot = kFirstSlot; slot <= kLastSlot; ++slot) {
        const auto entry = core::ReadOr<Address>(
            memory, vtable + static_cast<std::uint64_t>(slot) * sizeof(void*));
        if (!PointsIntoCode(memory, entry)) continue;

        auto candidate = reinterpret_cast<ProcessEventFn>(Raw(entry));

        if (!SlotAnswers(probe, candidate, 2, 3)) continue;

        // One correct answer is a coincidence waiting to be believed: a slot that writes
        // nothing leaves the block zeroed, and a probe expecting zero would pass. Two
        // different sums, neither of them zero, is an answer.
        if (!SlotAnswers(probe, candidate, 7, 11)) {
            core::LogDebug("slot {} matched once and then did not; ignoring", slot);
            continue;
        }

        info.vtable_index = slot;
        info.confidence   = 0.95f;
        info.evidence.push_back(std::format(
            "vtable slot {} returned the right answer twice through {}", slot, probe.path));

        core::LogInfo("UObject::ProcessEvent at vtable slot {}", slot);
        return info;
    }

    core::LogWarn("ProcessEvent: no slot in {}..{} answered correctly", kFirstSlot,
                  kLastSlot);
    return info;
}

} // namespace zircon::engine
