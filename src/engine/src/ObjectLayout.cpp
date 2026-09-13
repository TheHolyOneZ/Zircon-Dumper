#include "engine/ObjectLayout.h"
#include "core/Log.h"

#include <algorithm>
#include <format>
#include <map>
#include <set>

namespace zircon::engine {
namespace {

using core::Address;
using core::IsNull;
using core::Raw;

constexpr int kMaxOffset      = 0x48;
constexpr int kSampleTarget   = 64;
constexpr int kMaxOuterDepth  = 32;   // outer chains are shallow; this only guards loops

// Spread the samples across the array. The early slots are engine bootstrap objects and
// aren't representative of anything; taking the first N biases every derivation below.
std::vector<std::pair<std::int32_t, Address>> SampleObjects(core::IMemorySource& memory,
                                                            const ObjectArrayInfo& array,
                                                            int target) {
    std::vector<std::pair<std::int32_t, Address>> samples;
    if (array.num_elements <= 0) return samples;

    const std::int32_t stride = std::max(1, array.num_elements / (target * 3));
    for (std::int32_t index = 0;
         index < array.num_elements && static_cast<int>(samples.size()) < target;
         index += stride) {
        const auto object = ObjectAt(memory, array, index);
        if (IsNull(object)) continue;
        samples.emplace_back(index, object);
    }
    return samples;
}

// Read the object's own index and confirm the array agrees. A pointer into unrelated
// memory won't round-trip.
bool IsArrayObject(core::IMemorySource& memory, const ObjectArrayInfo& array,
                   Address candidate) {
    if (IsNull(candidate)) return false;

    std::int32_t index{};
    if (!core::ReadInto(memory, candidate + array.index_offset, index)) return false;
    if (index < 0 || index >= array.num_elements) return false;

    return Raw(ObjectAt(memory, array, index)) == Raw(candidate);
}

} // namespace

UObjectLayout DeriveObjectLayout(core::IMemorySource& memory,
                                 const ObjectArrayInfo& array,
                                 const NamePoolInfo& pool) {
    UObjectLayout layout;
    layout.index_offset = array.index_offset;

    const auto samples = SampleObjects(memory, array, kSampleTarget);
    if (samples.size() < 8) {
        core::LogWarn("only {} sampled objects; too few to derive the layout",
                      samples.size());
        return layout;
    }

    // --- pointer fields ---------------------------------------------------------------
    // ClassPrivate and OuterPrivate both hold pointers to other array objects, so counting
    // valid pointers can't tell them apart. Follow the link repeatedly and they diverge:
    //
    //   ClassPrivate converges to a fixed point. The class of UClass is UClass.
    //   OuterPrivate terminates at null. The outermost object is a package.
    //
    // Structural property of the engine rather than a version detail, so it survives forks
    // that shuffle the fields around.
    struct PointerStats {
        int valid{0};
        int null{0};
        int converged{0};
        Address fixed_point{};
    };
    std::map<int, PointerStats> pointer_stats;

    for (int offset = 8; offset <= kMaxOffset; offset += 8) {
        PointerStats stats;

        for (const auto& [index, object] : samples) {
            const auto target = core::ReadOr<Address>(memory, object + offset);
            if (IsNull(target)) { ++stats.null; continue; }
            if (!IsArrayObject(memory, array, target)) continue;
            ++stats.valid;

            Address current = target;
            for (int step = 0; step < kMaxOuterDepth; ++step) {
                const auto next = core::ReadOr<Address>(memory, current + offset);
                if (IsNull(next)) break;
                if (Raw(next) == Raw(current)) {
                    ++stats.converged;
                    stats.fixed_point = current;
                    break;
                }
                if (!IsArrayObject(memory, array, next)) break;
                current = next;
            }
        }

        // Has to look like an object pointer across most samples to be worth considering.
        if (stats.valid + stats.null >= static_cast<int>(samples.size()) * 3 / 4)
            pointer_stats[offset] = stats;
    }

    for (const auto& [offset, stats] : pointer_stats) {
        // ClassPrivate is never null on a live object and always converges.
        if (stats.null == 0 && stats.converged > static_cast<int>(samples.size()) / 2) {
            if (layout.class_offset < 0) {
                layout.class_offset  = offset;
                layout.uclass_object = stats.fixed_point;
            }
        }
    }

    for (const auto& [offset, stats] : pointer_stats) {
        if (offset == layout.class_offset) continue;
        // OuterPrivate: mostly valid object pointers, terminating rather than converging.
        if (stats.valid > 0 && stats.converged == 0) {
            if (layout.outer_offset < 0) layout.outer_offset = offset;
        }
    }

    // --- name field -------------------------------------------------------------------
    // Resolvability alone does NOT identify the name field. The upper 32 bits of the
    // vtable pointer are near-constant across objects in one module, so reading them as
    // an FName id resolves to the same valid name for nearly every sample and scores as
    // well as the real field.
    //
    // Two things separate the genuine field from a coincidence:
    //   1. diversity — real names differ across objects, a coincidence repeats
    //   2. the fixed point found above is UClass, so its name must be exactly "Class"
    //
    // The second is decisive, so it is used as a filter rather than a tiebreak.
    struct NameCandidate {
        int offset{};
        int hits{};
        std::size_t distinct{};
        bool names_the_fixed_point{false};
    };
    std::vector<NameCandidate> name_candidates;

    // Start past the vtable pointer: offsets 0-7 are not fields.
    for (int offset = 8; offset <= kMaxOffset; offset += 4) {
        if (offset == layout.class_offset || offset == layout.outer_offset) continue;
        if (offset == layout.index_offset) continue;

        NameCandidate candidate;
        candidate.offset = offset;

        std::set<std::string> seen_names;
        for (const auto& [index, object] : samples) {
            std::uint32_t comparison_index{};
            if (!core::ReadInto(memory, object + offset, comparison_index)) continue;

            const std::string name = ResolveName(memory, pool, comparison_index);
            if (name.empty()) continue;
            ++candidate.hits;
            seen_names.insert(name);
        }
        candidate.distinct = seen_names.size();

        if (candidate.hits < static_cast<int>(samples.size()) * 3 / 4) continue;

        // A single repeated name is the vtable-aliasing case.
        if (candidate.distinct < 2) continue;

        if (!IsNull(layout.uclass_object)) {
            std::uint32_t comparison_index{};
            if (core::ReadInto(memory, layout.uclass_object + offset, comparison_index))
                candidate.names_the_fixed_point =
                    ResolveName(memory, pool, comparison_index) == "Class";
        }

        name_candidates.push_back(std::move(candidate));
    }

    // Prefer a candidate that names the fixed point "Class"; fall back to the most
    // diverse one when no fixed point was found at all.
    std::sort(name_candidates.begin(), name_candidates.end(),
              [](const NameCandidate& a, const NameCandidate& b) {
                  if (a.names_the_fixed_point != b.names_the_fixed_point)
                      return a.names_the_fixed_point;
                  return a.distinct > b.distinct;
              });

    int best_name_hits = 0;
    if (!name_candidates.empty()) {
        const auto& best = name_candidates.front();
        layout.name_offset = best.offset;
        best_name_hits     = best.hits;

        if (!best.names_the_fixed_point && !IsNull(layout.uclass_object)) {
            core::LogWarn("name offset +{:#x} chosen on diversity alone; the class fixed "
                          "point is not named \"Class\", so names may be wrong",
                          best.offset);
        }
        if (name_candidates.size() > 1) {
            layout.evidence.push_back(std::format(
                "{} name-offset candidates; +{:#x} won with {} distinct names{}",
                name_candidates.size(), best.offset, best.distinct,
                best.names_the_fixed_point ? " and named the class fixed point" : ""));
        }
    }

    // --- scoring ----------------------------------------------------------------------
    int resolved_fields = 0;
    if (layout.class_offset >= 0) {
        ++resolved_fields;
        layout.evidence.push_back(std::format(
            "ClassPrivate at +{:#x}: converges to a fixed point at {:#x}",
            layout.class_offset, Raw(layout.uclass_object)));
    }
    if (layout.outer_offset >= 0) {
        ++resolved_fields;
        layout.evidence.push_back(std::format(
            "OuterPrivate at +{:#x}: chains terminate at null", layout.outer_offset));
    }
    if (layout.name_offset >= 0) {
        ++resolved_fields;
        layout.evidence.push_back(std::format(
            "NamePrivate at +{:#x}: {}/{} sampled objects resolved to printable names",
            layout.name_offset, best_name_hits, samples.size()));
    }

    // Finding all three fields is necessary but nowhere near sufficient: an earlier
    // version of this code selected a name offset inside the vtable pointer and still
    // reported every field resolved. So the headline number is capped well below
    // certainty until the end-to-end check below actually passes.
    layout.confidence = 0.45f * (static_cast<float>(resolved_fields) / 3.0f) *
                        std::min(1.0f, static_cast<float>(samples.size()) / kSampleTarget);

    // The decisive check. The class fixed point is UClass, so it must be named exactly
    // "Class". That single assertion exercises the object array, the name pool, the name
    // offset and the class offset together — if any one of them is wrong, it fails.
    if (layout.Valid() && !IsNull(layout.uclass_object)) {
        const auto name = GetObjectName(memory, layout, pool, layout.uclass_object);
        layout.evidence.push_back(std::format("class fixed point is named \"{}\"", name));

        if (name == "Class") {
            layout.confidence = std::min(0.99f, layout.confidence + 0.5f);
        } else {
            core::LogWarn("the class fixed point is named \"{}\" rather than \"Class\"; "
                          "the derived layout is probably wrong", name);
            layout.evidence.push_back(
                "end-to-end check FAILED: expected the class fixed point to be \"Class\"");
            layout.confidence *= 0.3f;
        }
    }

    core::LogInfo("UObject layout: index +{:#x}, class +{:#x}, name +{:#x}, outer +{:#x}",
                  layout.index_offset, layout.class_offset, layout.name_offset,
                  layout.outer_offset);
    return layout;
}

std::string GetObjectName(core::IMemorySource& memory, const UObjectLayout& layout,
                          const NamePoolInfo& pool, core::Address object) {
    if (IsNull(object) || layout.name_offset < 0) return {};

    std::uint32_t comparison_index{};
    std::int32_t  number{};
    if (!core::ReadInto(memory, object + layout.name_offset, comparison_index)) return {};
    core::ReadInto(memory, object + layout.name_offset + 4, number);

    return ResolveFName(memory, pool, comparison_index, number);
}

std::uint32_t GetObjectNameId(core::IMemorySource& memory, const UObjectLayout& layout,
                              core::Address object) {
    if (IsNull(object) || layout.name_offset < 0) return 0;
    return core::ReadOr<std::uint32_t>(memory, object + layout.name_offset);
}

core::Address GetObjectClass(core::IMemorySource& memory, const UObjectLayout& layout,
                             core::Address object) {
    if (IsNull(object) || layout.class_offset < 0) return {};
    return core::ReadOr<Address>(memory, object + layout.class_offset);
}

core::Address GetObjectOuter(core::IMemorySource& memory, const UObjectLayout& layout,
                             core::Address object) {
    if (IsNull(object) || layout.outer_offset < 0) return {};
    return core::ReadOr<Address>(memory, object + layout.outer_offset);
}

std::string GetObjectPathName(core::IMemorySource& memory, const UObjectLayout& layout,
                              const NamePoolInfo& pool, core::Address object) {
    if (IsNull(object)) return {};

    // Walk outwards first, then render inwards, so the package ends up leftmost.
    std::vector<std::string> parts;
    std::set<std::uint64_t> seen;

    for (Address current = object;
         !IsNull(current) && parts.size() < kMaxOuterDepth;
         current = GetObjectOuter(memory, layout, current)) {

        // A corrupt outer pointer could otherwise loop forever.
        if (!seen.insert(Raw(current)).second) break;

        std::string name = GetObjectName(memory, layout, pool, current);
        if (name.empty()) name = std::format("<{:#x}>", Raw(current));
        parts.push_back(std::move(name));
    }

    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        // Package names already start with "/", so a separator is only needed between
        // inner links.
        if (!path.empty()) path += '.';
        path += *it;
    }
    return path;
}

std::string GetObjectFullName(core::IMemorySource& memory, const UObjectLayout& layout,
                              const NamePoolInfo& pool, core::Address object) {
    if (IsNull(object)) return {};

    const auto class_object = GetObjectClass(memory, layout, object);
    const std::string class_name = GetObjectName(memory, layout, pool, class_object);
    const std::string path       = GetObjectPathName(memory, layout, pool, object);

    if (class_name.empty()) return path;
    return class_name + " " + path;
}

} // namespace zircon::engine
