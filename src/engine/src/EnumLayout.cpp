#include "engine/EnumLayout.h"
#include "core/Log.h"

#include <algorithm>
#include <format>

namespace zircon::engine {
namespace {

using core::Address;
using core::IsNull;
using core::Raw;

constexpr int kMaxProbe        = 0xC0;
constexpr int kMaxEntries      = 8192;
constexpr int kSampleTarget    = 200;

// Entry strides seen across engine versions. TPair<FName, int64> is 16 bytes when FName is
// 8, and a case-preserving or outline-number build widens it.
//
// Widening the set further was tried against UE 5.7, whose UEnum keeps its names somewhere
// this search doesn't reach. It found nothing there and made a false positive likelier
// everywhere else, so the narrow set stays.
constexpr int kStrides[] = {16, 24};

bool Readable(core::IMemorySource& memory, Address addr, std::size_t size = 8) {
    if (IsNull(addr) || Raw(addr) < 0x10000 || Raw(addr) >= 0x7FFFFFFFFFFFull) return false;
    std::uint8_t probe[64];
    const std::size_t want = std::min(size, sizeof(probe));
    return memory.Read(addr, probe, want) == want;
}

} // namespace


// The 5.7 shape: two tagged pointers and a count.
//
// Only searched once the interleaved shape has failed, so a build with the ordinary array
// is never at risk of matching here by accident.
//
// The tag makes it recognisable. Both pointers carry bit 0 and both are 8-byte aligned
// once it's cleared, which no ordinary pointer pair manages. Add a count that agrees with
// names which actually resolve and nothing else in a UEnum is shaped like this.
bool DeriveSplitEnumArrays(core::IMemorySource& memory, const NamePoolInfo& pool,
                           const UObjectLayout& object_layout,
                           const std::vector<Address>& enums, UEnumLayout& layout) {
    int best_offset = -1, best_hits = 0;

    for (int offset = object_layout.outer_offset + 8; offset <= kMaxProbe; offset += 8) {
        int hits = 0;

        for (const auto enum_object : enums) {
            const auto tagged_names  = core::ReadOr<std::uint64_t>(memory, enum_object + offset);
            const auto tagged_values = core::ReadOr<std::uint64_t>(memory, enum_object + offset + 8);
            const auto count = core::ReadOr<std::int32_t>(memory, enum_object + offset + 16);

            if (count <= 0 || count > kMaxEntries) continue;
            if ((tagged_names & 1) == 0 || (tagged_values & 1) == 0) continue;

            const auto names  = static_cast<Address>(tagged_names & ~1ull);
            const auto values = static_cast<Address>(tagged_values & ~1ull);

            // Clearing the tag must leave an aligned pointer. Otherwise the low bit was
            // data, and this isn't the field.
            if ((Raw(names) & 7) != 0 || (Raw(values) & 7) != 0) continue;
            if (!Readable(memory, names, 8) || !Readable(memory, values, 8)) continue;

            const int probe = std::min(count, 4);
            int good = 0;
            for (int i = 0; i < probe; ++i) {
                const auto id = core::ReadOr<std::uint32_t>(
                    memory, names + static_cast<std::uint64_t>(i) * 8);
                if (!ResolveName(memory, pool, id).empty()) ++good;
            }
            if (good == probe) ++hits;
        }

        if (hits > best_hits) {
            best_hits   = hits;
            best_offset = offset;
        }
    }

    if (best_offset < 0 || best_hits < static_cast<int>(enums.size()) / 2) return false;

    layout.split_arrays = true;
    layout.names_array  = best_offset;
    layout.values_array = best_offset + 8;
    layout.count_at     = best_offset + 16;
    layout.confidence   = 0.9f;
    layout.evidence.push_back(std::format(
        "UEnum names and values are separate arrays: tagged pointers at +{:#x} and +{:#x}, "
        "count at +{:#x}, resolving for {}/{} sampled enums",
        best_offset, best_offset + 8, best_offset + 16, best_hits, enums.size()));

    core::LogInfo("UEnum layout: split arrays, names +{:#x}, values +{:#x}, count +{:#x}",
                  best_offset, best_offset + 8, best_offset + 16);
    return true;
}

UEnumLayout DeriveEnumLayout(core::IMemorySource& memory,
                             const ObjectArrayInfo& array,
                             const NamePoolInfo& pool,
                             const UObjectLayout& object_layout) {
    UEnumLayout layout;

    std::vector<Address> enums;
    for (std::int32_t index = 0;
         index < array.num_elements && static_cast<int>(enums.size()) < kSampleTarget;
         ++index) {
        const auto object = ObjectAt(memory, array, index);
        if (IsNull(object)) continue;
        if (GetClassName(memory, object_layout, pool, object) != "Enum") continue;
        enums.push_back(object);
    }

    if (enums.size() < 8) {
        core::LogWarn("only {} UEnum objects sampled; cannot derive the enum layout",
                      enums.size());
        return layout;
    }

    // A TArray is {T* Data; int32 Num; int32 Max}. Plausible-looking numbers prove
    // nothing; following Data and getting resolvable names does. An enum with unreadable
    // entries isn't the Names array, whatever its header claims.
    int best_offset = -1, best_stride = 16, best_hits = 0;

    for (int offset = object_layout.outer_offset + 8; offset <= kMaxProbe; offset += 8) {
        for (const int stride : kStrides) {
            int hits = 0;

            for (const auto enum_object : enums) {
                const auto data = core::ReadOr<Address>(memory, enum_object + offset);
                const auto count = core::ReadOr<std::int32_t>(memory, enum_object + offset + 8);
                const auto capacity =
                    core::ReadOr<std::int32_t>(memory, enum_object + offset + 12);

                if (count <= 0 || count > kMaxEntries) continue;
                if (capacity < count || capacity > kMaxEntries) continue;
                if (!Readable(memory, data, static_cast<std::size_t>(stride))) continue;

                // A few entries is plenty. Wrong candidates fail on the first, right ones
                // never fail.
                int good = 0;
                const int probe = std::min(count, 4);
                for (int i = 0; i < probe; ++i) {
                    const auto id =
                        core::ReadOr<std::uint32_t>(memory, data + static_cast<std::uint64_t>(i) * stride);
                    if (!ResolveName(memory, pool, id).empty()) ++good;
                }
                if (good == probe) ++hits;
            }

            if (hits > best_hits) {
                best_hits   = hits;
                best_offset = offset;
                best_stride = stride;
            }
        }
    }

    if (best_offset < 0 || best_hits < static_cast<int>(enums.size()) * 3 / 4) {
        // No interleaved array. Try the shape UE 5.7 uses before giving up.
    if (DeriveSplitEnumArrays(memory, pool, object_layout, enums, layout)) return layout;

    core::LogWarn("could not identify UEnum::Names ({}/{} enums matched)",
                      best_hits, enums.size());
        return layout;
    }

    layout.names_array = best_offset;
    layout.pair_stride = best_stride;
    layout.confidence  = std::min(0.96f,
                                  0.5f + 0.5f * static_cast<float>(best_hits) / enums.size());
    layout.evidence.push_back(std::format(
        "UEnum::Names at +{:#x}, {} bytes per entry: {}/{} sampled enums resolve their "
        "first entries", best_offset, best_stride, best_hits, enums.size()));

    core::LogInfo("UEnum layout: names +{:#x}, stride {}", best_offset, best_stride);
    return layout;
}

std::vector<std::pair<std::string, std::int64_t>> GetEnumValues(
    core::IMemorySource& memory, const UEnumLayout& layout, const NamePoolInfo& pool,
    Address enum_object) {

    std::vector<std::pair<std::string, std::int64_t>> values;
    if (IsNull(enum_object) || !layout.Valid()) return values;

    // UE 5.7 keeps names and values in two arrays instead of one array of pairs,
    // each behind a pointer with its low bit set as a tag.
    if (layout.split_arrays) {
        const auto tagged_names  = core::ReadOr<std::uint64_t>(memory, enum_object + layout.names_array);
        const auto tagged_values = core::ReadOr<std::uint64_t>(memory, enum_object + layout.values_array);
        const auto count = core::ReadOr<std::int32_t>(memory, enum_object + layout.count_at);

        if (count <= 0 || count > kMaxEntries) return values;
        if ((tagged_names & 1) == 0 || (tagged_values & 1) == 0) return values;

        const auto names  = static_cast<Address>(tagged_names & ~1ull);
        const auto value_data = static_cast<Address>(tagged_values & ~1ull);

        values.reserve(static_cast<std::size_t>(count));
        for (std::int32_t i = 0; i < count; ++i) {
            const auto at = static_cast<std::uint64_t>(i) * 8;

            const auto id     = core::ReadOr<std::uint32_t>(memory, names + at);
            const auto number = core::ReadOr<std::int32_t>(memory, names + at + 4);
            std::string name  = ResolveFName(memory, pool, id, number);
            if (name.empty()) continue;

            values.emplace_back(std::move(name),
                                core::ReadOr<std::int64_t>(memory, value_data + at));
        }
        return values;
    }

    const auto data  = core::ReadOr<Address>(memory, enum_object + layout.names_array);
    const auto count = core::ReadOr<std::int32_t>(memory, enum_object + layout.names_array + 8);
    if (IsNull(data) || count <= 0 || count > kMaxEntries) return values;

    values.reserve(static_cast<std::size_t>(count));
    for (std::int32_t i = 0; i < count; ++i) {
        const Address entry = data + static_cast<std::uint64_t>(i) * layout.pair_stride;

        const auto id     = core::ReadOr<std::uint32_t>(memory, entry);
        const auto number = core::ReadOr<std::int32_t>(memory, entry + 4);
        std::string name  = ResolveFName(memory, pool, id, number);
        if (name.empty()) continue;

        // The int64 sits at the end of the pair, so its position tracks the stride.
        const auto value = core::ReadOr<std::int64_t>(memory, entry + (layout.pair_stride - 8));
        values.emplace_back(std::move(name), value);
    }
    return values;
}

} // namespace zircon::engine
