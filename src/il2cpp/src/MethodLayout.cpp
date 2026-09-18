#include "il2cpp/MethodLayout.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace zircon::il2cpp {
namespace {

using core::Address;
using core::Raw;

// MethodInfo's head is small, three pointers at most before the name. 0x80 covers every
// build we've seen with room to spare; scanning further just invites coincidences.
constexpr std::int32_t kMaxScan = 0x80;
constexpr std::size_t  kSlots   = kMaxScan / 8;

// Fraction a slot has to clear to count as anchored. Not 100% -- there are always a few
// methods with a null body or class, and one of those shouldn't veto a slot that's right
// everywhere else.
constexpr double kAnchorRatio = 0.90;

// Fraction of a slot's values that have to sit in an executable section for it to count as
// code. Metadata pointers land in .rdata and score 0; code pointers score ~100. No middle.
constexpr double kCodeRatio = 0.90;

// How much .pdata has to back that up, across the code slots together. Sanity check that
// these are function starts at all, not a per-slot gate: leaf functions have no unwind data
// and invoker thunks are mostly leaves. Real builds: 20-80%.
constexpr double kEntryPointFloor = 0.25;

// How much more the invoker has to repeat than the body. A ratio, not a threshold -- the
// absolute numbers move with the sample, which one repeats more doesn't.
constexpr double kSharingMargin = 2.0;

// Below this there is not enough of a sample for "varies within its group" to mean anything.
constexpr std::size_t kMinSample  = 32;
constexpr std::size_t kMinClasses = 4;
constexpr std::size_t kMinGroup   = 4;

struct Slot {
    std::int32_t offset{0};
    double       in_module_ratio{0.0};
    double       code_ratio{0.0};
    double       function_start_ratio{0.0};
    double       distinct_ratio{0.0};
    double       within_group_variety{0.0};
};

std::string Percent(double value) {
    return std::format("{:.0f}%", value * 100.0);
}

} // namespace

MethodInfoLayout DeriveMethodInfoLayout(core::IMemorySource& memory,
                                        std::span<const MethodProbe> sample,
                                        Address module_base,
                                        std::uint64_t module_size,
                                        std::span<const std::uint32_t> function_starts,
                                        std::span<const core::RegionInfo> executable) {
    MethodInfoLayout layout;

    if (executable.empty()) {
        layout.refusal = "the module reports no executable sections, so there is no way to "
                         "tell a pointer to code from any other pointer into it";
        return layout;
    }

    // No .pdata means nothing separates "pointer into the module" from "start of a
    // function", and the pointers ahead of the body are into the module too.
    if (function_starts.empty()) {
        layout.refusal = "the module declares no exception directory, so there is no "
                         "independent way to tell a code pointer from any other pointer "
                         "into the same module";
        return layout;
    }

    if (sample.size() < kMinSample) {
        layout.refusal = std::format("{} methods is too small a sample; {} is the minimum",
                                     sample.size(), kMinSample);
        return layout;
    }

    {
        std::unordered_set<std::uint64_t> classes;
        for (const auto& probe : sample) classes.insert(Raw(probe.klass));
        if (classes.size() < kMinClasses) {
            layout.refusal = std::format(
                "sample spans {} classes; with fewer than {} a slot can hold the same value "
                "for a reason that has nothing to do with being the class pointer",
                classes.size(), kMinClasses);
            return layout;
        }
    }

    // One read per method, everything else works off the copy.
    std::vector<std::array<std::uint64_t, kSlots>> words(sample.size());
    std::size_t readable = 0;
    for (std::size_t i = 0; i < sample.size(); ++i) {
        words[i].fill(0);
        if (memory.Read(sample[i].method, words[i].data(), kMaxScan) == kMaxScan) ++readable;
    }
    if (readable * 100 < sample.size() * 90) {
        layout.refusal = std::format("only {} of {} MethodInfo blocks were readable",
                                     readable, sample.size());
        return layout;
    }

    const auto anchor = [&](Address MethodProbe::* field) -> std::int32_t {
        for (std::size_t slot = 0; slot < kSlots; ++slot) {
            std::size_t hits = 0, eligible = 0;
            for (std::size_t i = 0; i < sample.size(); ++i) {
                const std::uint64_t want = Raw(sample[i].*field);
                if (want == 0) continue;
                ++eligible;
                if (words[i][slot] == want) ++hits;
            }
            if (eligible >= kMinSample &&
                static_cast<double>(hits) >= kAnchorRatio * static_cast<double>(eligible))
                return static_cast<std::int32_t>(slot * 8);
        }
        return -1;
    };

    layout.name        = anchor(&MethodProbe::name);
    layout.klass       = anchor(&MethodProbe::klass);
    layout.return_type = anchor(&MethodProbe::return_type);

    if (layout.name < 0) {
        layout.refusal = "no slot in the head of MethodInfo holds the pointer "
                         "il2cpp_method_get_name returns, so nothing else can be placed "
                         "relative to it";
        return layout;
    }

    // Token. Corroboration, not load-bearing: confirms this really is a MethodInfo.
    for (std::int32_t offset = 0; offset + 4 <= kMaxScan && layout.token < 0; offset += 4) {
        std::size_t hits = 0, eligible = 0;
        for (std::size_t i = 0; i < sample.size(); ++i) {
            if (sample[i].token == 0) continue;
            ++eligible;
            std::uint32_t value = 0;
            std::memcpy(&value,
                        reinterpret_cast<const std::uint8_t*>(words[i].data()) + offset,
                        sizeof(value));
            if (value == sample[i].token) ++hits;
        }
        if (eligible >= kMinSample &&
            static_cast<double>(hits) >= kAnchorRatio * static_cast<double>(eligible))
            layout.token = offset;
    }

    // Every code pointer sits below `name`. Not a per-version assumption -- MethodInfo has
    // only ever grown by appending, so the code pointers start at 0 and end at the name.
    const std::size_t code_slots = static_cast<std::size_t>(layout.name) / 8;
    if (code_slots < 2) {
        layout.refusal = std::format(
            "MethodInfo::name is at {:#x}, leaving {} slot(s) ahead of it; with fewer than "
            "two there is no invoker to tell the body apart from",
            layout.name, code_slots);
        return layout;
    }

    // Same arg count + same static-ness = same invoker thunk. That grouping is the
    // discriminator.
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < sample.size(); ++i) {
        const std::uint64_t key = (static_cast<std::uint64_t>(sample[i].param_count) << 1) |
                                  (sample[i].is_instance ? 1u : 0u);
        groups[key].push_back(i);
    }

    std::vector<Slot> slots;
    for (std::size_t slot = 0; slot < code_slots; ++slot) {
        Slot stats;
        stats.offset = static_cast<std::int32_t>(slot * 8);

        std::size_t at_start = 0, in_module = 0, in_code = 0;
        std::unordered_set<std::uint64_t> distinct;
        for (std::size_t i = 0; i < sample.size(); ++i) {
            const std::uint64_t value = words[i][slot];
            distinct.insert(value);
            if (value < Raw(module_base) || value >= Raw(module_base) + module_size) continue;
            ++in_module;

            bool executable_here = false;
            for (const auto& region : executable) {
                if (value >= Raw(region.base) && value < Raw(region.base) + region.size) {
                    executable_here = true;
                    break;
                }
            }
            if (!executable_here) continue;
            ++in_code;

            const auto rva = static_cast<std::uint32_t>(value - Raw(module_base));
            if (std::binary_search(function_starts.begin(), function_starts.end(), rva))
                ++at_start;
        }

        const auto total = static_cast<double>(sample.size());
        stats.in_module_ratio      = static_cast<double>(in_module) / total;
        stats.code_ratio           = static_cast<double>(in_code) / total;
        stats.function_start_ratio = static_cast<double>(at_start) / total;
        stats.distinct_ratio       = static_cast<double>(distinct.size()) / total;

        double variety_sum = 0.0;
        std::size_t counted = 0;
        for (const auto& [key, members] : groups) {
            if (members.size() < kMinGroup) continue;
            std::unordered_set<std::uint64_t> group_distinct;
            for (const std::size_t i : members) group_distinct.insert(words[i][slot]);
            variety_sum += static_cast<double>(group_distinct.size()) /
                           static_cast<double>(members.size());
            ++counted;
        }
        stats.within_group_variety = counted ? variety_sum / static_cast<double>(counted) : 0.0;

        slots.push_back(stats);
    }

    // Per-slot numbers go in the evidence whatever happens next. "none qualified" on its
    // own leaves the next person guessing which constraint bit.
    for (const auto& slot : slots) {
        layout.evidence.push_back(std::format(
            "slot {:#x}: {} in an executable section, {} at a function entry point, "
            "{} distinct, varies {} within one argument shape",
            slot.offset, Percent(slot.code_ratio), Percent(slot.function_start_ratio),
            Percent(slot.distinct_ratio), Percent(slot.within_group_variety)));
    }

    // A slot holds code when its values are in an executable section. Nulls and metadata
    // pointers score zero. Presence constraint, on purpose.
    std::vector<Slot> code;
    double best_entry_points = 0.0;
    for (const auto& slot : slots) {
        if (slot.code_ratio < kCodeRatio) continue;
        code.push_back(slot);
        best_entry_points = std::max(best_entry_points, slot.function_start_ratio);
    }

    if (!code.empty() && best_entry_points < kEntryPointFloor) {
        layout.refusal = std::format(
            "the slots that hold code point at function entry points only {} of the time; "
            "that is low enough that they may be addresses inside .text rather than the "
            "starts of anything",
            Percent(best_entry_points));
        return layout;
    }

    if (code.size() < 2 || code.size() > 3) {
        layout.refusal = std::format(
            "{} of the {} slot(s) ahead of MethodInfo::name hold function entry points; two "
            "or three is the shape this struct has ever had, and outside that range nothing "
            "here knows what it is reading",
            code.size(), code_slots);
        return layout;
    }

    // Which slot is which is structural: invoker is the last pointer before the name, body
    // is the first. That alone is the kind of plausible-but-unchecked answer that keeps
    // burning us, so it's a claim to verify, not a conclusion.
    const Slot& invoker = code.back();
    const Slot& body    = code.front();

    if (invoker.offset != layout.name - 8) {
        layout.refusal = std::format(
            "the last code pointer is at {:#x} but MethodInfo::name is at {:#x}; the invoker "
            "has always sat immediately before the name, so this is not the struct being "
            "read",
            invoker.offset, layout.name);
        return layout;
    }

    // The verification. Only thing between us and reporting an invoker thunk as a body.
    //
    // An invoker is shared -- one thunk per signature shape. A body is the method's own. So
    // the last slot has to repeat measurably more than the first, both ways repetition shows
    // up. Comparison, not threshold: absolute figures move with the sample, the order
    // doesn't.
    if (invoker.distinct_ratio * kSharingMargin > body.distinct_ratio) {
        layout.refusal = std::format(
            "the slot before MethodInfo::name holds {} distinct values against the first "
            "slot's {}; an invoker is shared by every method of one shape and should repeat "
            "far more than a body does, so these two are not what they are being taken for",
            Percent(invoker.distinct_ratio), Percent(body.distinct_ratio));
        return layout;
    }

    if (invoker.within_group_variety >= body.within_group_variety) {
        layout.refusal = std::format(
            "within one argument shape the slot before MethodInfo::name varies {} and the "
            "first slot varies {}; an invoker is what does not vary there, so the two slots "
            "are the wrong way round or neither is what it seems",
            Percent(invoker.within_group_variety), Percent(body.within_group_variety));
        return layout;
    }

    layout.invoker = invoker.offset;
    layout.body    = body.offset;
    if (code.size() == 3) layout.virtual_body = code[1].offset;

    layout.evidence.push_back(std::format(
        "anchored on {} methods across {} classes and {} argument shapes",
        sample.size(), [&] {
            std::unordered_set<std::uint64_t> classes;
            for (const auto& probe : sample) classes.insert(Raw(probe.klass));
            return classes.size();
        }(), groups.size()));
    layout.evidence.push_back(std::format(
        "name at {:#x}, klass at {:#x}, return type at {:#x}, token at {:#x}, every one by "
        "exact match against the pointer the API handed back",
        layout.name, layout.klass, layout.return_type, layout.token));
    layout.evidence.push_back(std::format(
        "body at {:#x}, invoker at {:#x}: the invoker repeats {:.1f}x more across the sample "
        "and varies less inside one argument shape, which is what being shared by every "
        "method of that shape looks like",
        layout.body, layout.invoker,
        invoker.distinct_ratio > 0.0 ? body.distinct_ratio / invoker.distinct_ratio : 0.0));

    if (layout.virtual_body >= 0) {
        layout.evidence.push_back(std::format(
            "a third code pointer at {:#x}; Unity added one in 2021.2 for value-type adjustor "
            "thunks, and the method's own code is the first of the three",
            layout.virtual_body));
    }

    return layout;
}

} // namespace zircon::il2cpp
