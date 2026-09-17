#include "engine/StructLayout.h"
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

// UStruct starts after UObject and UField::Next, so nothing interesting sits below 0x28 in
// any layout we care about. Upper bound is generous; a licensee build that inserts members
// still keeps these fields well inside it.
constexpr int kMinStructOffset = 0x28;
constexpr int kMaxStructOffset = 0x98;

constexpr int kClassSampleTarget = 256;
constexpr int kMaxChainDepth     = 64;

bool Readable(core::IMemorySource& memory, Address addr, std::size_t size = 8) {
    if (IsNull(addr) || Raw(addr) < 0x10000 || Raw(addr) >= 0x7FFFFFFFFFFFull) return false;
    std::uint8_t probe[64];
    const std::size_t want = std::min(size, sizeof(probe));
    return memory.Read(addr, probe, want) == want;
}

bool IsArrayObject(core::IMemorySource& memory, const ObjectArrayInfo& array,
                   Address candidate) {
    if (IsNull(candidate)) return false;
    std::int32_t index{};
    if (!core::ReadInto(memory, candidate + array.index_offset, index)) return false;
    if (index < 0 || index >= array.num_elements) return false;
    return Raw(ObjectAt(memory, array, index)) == Raw(candidate);
}

} // namespace

std::string GetClassName(core::IMemorySource& memory, const UObjectLayout& layout,
                         const NamePoolInfo& pool, Address object) {
    return GetObjectName(memory, layout, pool, GetObjectClass(memory, layout, object));
}

ObjectKind ClassifyObject(core::IMemorySource& memory, const UObjectLayout& object_layout,
                          const UStructLayout& struct_layout, const NamePoolInfo& pool,
                          Address object) {
    if (IsNull(object)) return ObjectKind::Other;

    // The meta-class is what the object *is*, and walking its super chain is what lets
    // BlueprintGeneratedClass count as a class. It derives from UClass while sharing
    // nothing with it by name.
    Address meta = GetObjectClass(memory, object_layout, object);
    std::set<std::uint64_t> seen;

    for (int depth = 0; depth < 32 && !IsNull(meta); ++depth) {
        if (!seen.insert(Raw(meta)).second) break;   // guards a corrupt chain

        const std::string name = GetObjectName(memory, object_layout, pool, meta);
        if (name == "Class")        return ObjectKind::Class;
        if (name == "ScriptStruct") return ObjectKind::ScriptStruct;
        if (name == "Enum")         return ObjectKind::Enum;
        if (name == "Function")     return ObjectKind::Function;
        if (name == "Package")      return ObjectKind::Package;

        if (struct_layout.super_struct < 0) break;
        meta = GetSuperStruct(memory, struct_layout, meta);
    }
    return ObjectKind::Other;
}

UStructLayout DeriveStructLayout(core::IMemorySource& memory,
                                 const ObjectArrayInfo& array,
                                 const NamePoolInfo& pool,
                                 const UObjectLayout& object_layout) {
    UStructLayout layout;

    // Anything whose own class is named "Class". Spread across the array to pick up engine
    // and game classes as well as bootstrap ones.
    std::vector<Address> classes;
    for (std::int32_t index = 0;
         index < array.num_elements && static_cast<int>(classes.size()) < kClassSampleTarget;
         ++index) {
        const auto object = ObjectAt(memory, array, index);
        if (IsNull(object)) continue;
        if (GetClassName(memory, object_layout, pool, object) != "Class") continue;
        classes.push_back(object);
    }

    if (classes.size() < 16) {
        core::LogWarn("only {} UClass objects sampled; too few to derive UStruct layout",
                      classes.size());
        return layout;
    }

    // --- SuperStruct ------------------------------------------------------------------
    // Every class chain ends at /Script/CoreUObject.Object, which has no super of its own.
    // That's a property of the type system, not of any build, so scoring candidates
    // on "does the chain end at an object named Object with a null super" finds the field
    // without knowing a version.
    int   best_super       = -1;
    int   best_super_score = 0;
    Address object_class{};

    for (int offset = kMinStructOffset; offset <= kMaxStructOffset; offset += 8) {
        int reached_root = 0;
        Address terminal{};

        for (const auto klass : classes) {
            Address current = klass;
            Address last    = klass;
            std::set<std::uint64_t> seen;

            bool sane = true;
            for (int depth = 0; depth < kMaxChainDepth; ++depth) {
                const auto next = core::ReadOr<Address>(memory, current + offset);
                if (IsNull(next)) break;
                if (!IsArrayObject(memory, array, next)) { sane = false; break; }
                if (!seen.insert(Raw(next)).second)      { sane = false; break; }
                last    = next;
                current = next;
            }
            if (!sane) continue;

            if (GetObjectName(memory, object_layout, pool, last) == "Object") {
                ++reached_root;
                terminal = last;
            }
        }

        if (reached_root > best_super_score) {
            best_super_score = reached_root;
            best_super       = offset;
            object_class     = terminal;
        }
    }

    // Most sampled classes have to reach the root. A partial match means we latched onto
    // some other pointer that happens to chain.
    if (best_super_score >= static_cast<int>(classes.size()) * 3 / 4) {
        layout.super_struct = best_super;
        layout.object_class = object_class;
        layout.evidence.push_back(std::format(
            "SuperStruct at +{:#x}: {}/{} class chains terminate at "
            "/Script/CoreUObject.Object", best_super, best_super_score, classes.size()));
    } else {
        core::LogWarn("could not identify SuperStruct ({}/{} chains reached the root)",
                      best_super_score, classes.size());
        return layout;
    }

    // Everything else in UStruct is declared after SuperStruct, so searching below it only
    // turns up members of the bases. Concretely: UE5's UStruct also inherits
    // FStructBaseChain, whose heap pointer and depth counter sit immediately *before*
    // SuperStruct. An unconstrained search takes the high half of that pointer for
    // PropertiesSize, since it's constant across objects and therefore looks stable, and the
    // depth counter for MinAlignment, since small integers look like alignments.
    const int search_from = layout.super_struct + 8;

    // --- PropertiesSize ---------------------------------------------------------------
    // Two independent constraints, both required:
    //   1. a derived class is never smaller than its base
    //   2. /Script/CoreUObject.Object reports the size of a UObject
    // The second is an anchor, not a heuristic, and it's what rules out fields that merely
    // happen to be monotonic.
    //
    // Knowing what a UObject weighs is the hard half. For stock UE, OuterPrivate is the
    // last member, so the separately derived object layout gives it exactly. A fork that
    // appends its own fields to UObject breaks that, and then nothing matches at all.
    //
    // Neighbouring fields do not rescue it. UField is a UObject plus one pointer, so Next
    // would give the size -- except a build carrying UE5's FStructBaseChain puts sixteen
    // bytes between Next and SuperStruct, and working back from SuperStruct overshoots by
    // exactly that much. Both routes assume a tail that a fork is free to change.
    //
    // So: the exact figure first, which keeps every target that already worked identical.
    // Only when that finds nothing, anchor on things that say nothing about the tail:
    //   - at least outer_offset + 8, because those members demonstrably exist
    //   - 8-aligned, because UObject holds pointers
    //   - no class smaller than the root, since every class derives from UObject
    //   - a real alignment in the next dword
    const std::int32_t stock_object_size =
        static_cast<std::int32_t>(object_layout.outer_offset + 8);

    // A fork can append to UObject; it cannot append a kilobyte and stay an engine.
    constexpr std::int32_t kMaxObjectTailGrowth = 256;
    constexpr std::int32_t kMaxStructBytes      = 1 << 20;

    const auto sane_size = [](std::int32_t size) {
        return size > 0 && size <= kMaxStructBytes;
    };

    // How many class/super pairs agree that a derived type is no smaller than its base.
    const auto monotonic_score = [&](int offset) {
        int consistent = 0;
        for (const auto klass : classes) {
            const auto super = core::ReadOr<Address>(memory, klass + layout.super_struct);
            if (IsNull(super)) continue;

            std::int32_t size{}, super_size{};
            if (!core::ReadInto(memory, klass + offset, size)) continue;
            if (!core::ReadInto(memory, super + offset, super_size)) continue;
            if (!sane_size(size) || !sane_size(super_size)) continue;
            if (size < super_size) continue;
            ++consistent;
        }
        return consistent;
    };

    // Nothing derives from UObject and comes out smaller than it. Independent of where
    // UObject ends, which is the whole point.
    const auto nothing_smaller_than_root = [&](int offset, std::int32_t root_size) {
        int checked = 0;
        int not_smaller = 0;
        for (const auto klass : classes) {
            std::int32_t size{};
            if (!core::ReadInto(memory, klass + offset, size)) continue;
            if (!sane_size(size)) continue;
            ++checked;
            if (size >= root_size) ++not_smaller;
        }
        return checked > 0 && not_smaller == checked;
    };

    // MinAlignment sits in the dword after PropertiesSize. Used here as a second opinion on
    // the candidate rather than only as a follow-up read, because a field that is monotonic
    // by accident is unlikely to be followed by a plausible alignment for every class.
    const auto alignment_follows = [&](int offset) {
        const auto plausible = [&](bool as_u16) {
            int count = 0;
            for (const auto klass : classes) {
                std::uint32_t value{};
                if (as_u16) {
                    std::uint16_t narrow{};
                    if (!core::ReadInto(memory, klass + offset + 4, narrow)) continue;
                    value = narrow;
                } else {
                    if (!core::ReadInto(memory, klass + offset + 4, value)) continue;
                }
                if (value > 0 && value <= 64 && (value & (value - 1)) == 0) ++count;
            }
            return count;
        };
        const int needed = static_cast<int>(classes.size()) * 3 / 4;
        return plausible(false) >= needed || plausible(true) >= needed;
    };

    int          best_size_offset = -1;
    int          best_size_score  = 0;
    std::int32_t best_root_size   = 0;

    for (int offset = search_from; offset <= kMaxStructOffset; offset += 4) {
        // Anchor first. One read, and it kills almost every candidate.
        std::int32_t root_size{};
        if (!core::ReadInto(memory, layout.object_class + offset, root_size)) continue;
        if (root_size != stock_object_size) continue;

        const int consistent = monotonic_score(offset);
        if (consistent > best_size_score) {
            best_size_score  = consistent;
            best_size_offset = offset;
            best_root_size   = root_size;
        }
    }

    const bool stock_anchor_held =
        best_size_offset >= 0 && best_size_score >= static_cast<int>(classes.size()) / 2;

    if (!stock_anchor_held) {
        best_size_offset = -1;
        best_size_score  = 0;

        for (int offset = search_from; offset <= kMaxStructOffset; offset += 4) {
            std::int32_t root_size{};
            if (!core::ReadInto(memory, layout.object_class + offset, root_size)) continue;

            if (root_size < stock_object_size) continue;
            if (root_size > stock_object_size + kMaxObjectTailGrowth) continue;
            if (root_size % 8 != 0) continue;
            if (!nothing_smaller_than_root(offset, root_size)) continue;
            if (!alignment_follows(offset)) continue;

            const int consistent = monotonic_score(offset);
            if (consistent > best_size_score) {
                best_size_score  = consistent;
                best_size_offset = offset;
                best_root_size   = root_size;
            }
        }

        // One assumption fewer held here, so ask for more of the sample to agree than the
        // stock path does before believing it.
        if (best_size_offset >= 0 &&
            best_size_score < static_cast<int>(classes.size()) * 3 / 4) {
            best_size_offset = -1;
        }
    }

    if (best_size_offset >= 0) {
        layout.properties_size = best_size_offset;
        layout.object_size     = best_root_size;
        layout.extends_uobject = best_root_size != stock_object_size;

        if (!layout.extends_uobject) {
            layout.evidence.push_back(std::format(
                "PropertiesSize at +{:#x}: /Script/CoreUObject.Object reports {} bytes, and "
                "{} class/super pairs satisfy size(derived) >= size(base)",
                best_size_offset, best_root_size, best_size_score));
        } else {
            core::LogInfo("this build extends UObject: sizeof(UObject) is {} bytes, {} more "
                          "than the members alone account for",
                          best_root_size, best_root_size - stock_object_size);
            layout.evidence.push_back(std::format(
                "PropertiesSize at +{:#x}: /Script/CoreUObject.Object reports {} bytes -- {} "
                "more than its members account for, so this build extends UObject. No class "
                "is smaller than the root, an alignment follows, and {} class/super pairs "
                "satisfy size(derived) >= size(base)",
                best_size_offset, best_root_size, best_root_size - stock_object_size,
                best_size_score));
        }

        // MinAlignment follows PropertiesSize, though not always as a full int32. At least
        // one shipping UE5 build keeps 8 in the low half of that dword with other data
        // above. Natural width first, then the narrower one, before reporting an
        // alignment of 65544.
        const int candidate = best_size_offset + 4;

        auto count_valid = [&](bool as_u16) {
            int plausible = 0;
            for (const auto klass : classes) {
                std::uint32_t value{};
                if (as_u16) {
                    std::uint16_t narrow{};
                    if (!core::ReadInto(memory, klass + candidate, narrow)) continue;
                    value = narrow;
                } else {
                    if (!core::ReadInto(memory, klass + candidate, value)) continue;
                }
                if (value > 0 && value <= 64 && (value & (value - 1)) == 0) ++plausible;
            }
            return plausible;
        };

        const int needed    = static_cast<int>(classes.size()) * 3 / 4;
        const int as_int32  = count_valid(false);
        const int as_uint16 = count_valid(true);

        if (as_int32 >= needed) {
            layout.min_alignment = candidate;
            layout.evidence.push_back(std::format(
                "MinAlignment at +{:#x} as int32: {} classes hold a small power of two",
                candidate, as_int32));
        } else if (as_uint16 >= needed) {
            layout.min_alignment        = candidate;
            layout.min_alignment_is_u16 = true;
            layout.evidence.push_back(std::format(
                "MinAlignment at +{:#x} as uint16: {} classes hold a small power of two "
                "(the full int32 does not, so this build stores it narrow)",
                candidate, as_uint16));
        }
    }

    // --- Children and ChildProperties -------------------------------------------------
    // Both are linked-list heads, and what they point at tells them apart. Children holds
    // UObjects (functions, enums) so its targets are in the object array; ChildProperties
    // holds FFields, which are not UObjects and never will be. Classifying by membership
    // beats guessing a declaration order that has moved between versions.
    //
    // Scored, not first-match. Plenty of classes legitimately have a null Children, so an
    // absolute threshold either misses the field or accepts an unrelated pointer.
    //
    // Declaration order settles the rest. UStruct declares SuperStruct, Children,
    // ChildProperties, PropertiesSize, MinAlignment, Script, then the PropertyLink chain.
    // PropertyLink is also an FProperty pointer and is non-null for *more* classes than
    // ChildProperties, since it includes inherited properties. Take the highest-scoring
    // FField pointer and you reliably get the wrong field.
    //
    // So: lowest qualifying offset above SuperStruct is the declared one. Safe only because
    // the search is already floored at SuperStruct + 8. Without that floor it picks up
    // members of the bases instead.
    std::vector<std::pair<int, int>> children_candidates;   // offset, score
    std::vector<std::pair<int, int>> property_candidates;

    for (int offset = search_from; offset <= kMaxStructOffset; offset += 8) {
        if (offset == layout.properties_size) continue;

        int in_array = 0, out_of_array = 0;

        for (const auto klass : classes) {
            const auto head = core::ReadOr<Address>(memory, klass + offset);
            if (IsNull(head)) continue;
            if (!Readable(memory, head, 32)) continue;

            if (IsArrayObject(memory, array, head)) {
                ++in_array;
            } else {
                // An FField has a vtable. Unrelated heap data usually doesn't.
                const auto vtable = core::ReadOr<Address>(memory, head);
                if (Readable(memory, vtable, 8)) ++out_of_array;
            }
        }

        if (in_array > out_of_array * 4)      children_candidates.emplace_back(offset, in_array);
        else if (out_of_array > in_array * 4) property_candidates.emplace_back(offset, out_of_array);
    }

    const int minimum_heads = static_cast<int>(classes.size()) / 8;

    for (const auto& [offset, score] : children_candidates) {
        if (score < minimum_heads) continue;
        layout.children = offset;
        layout.evidence.push_back(std::format(
            "Children at +{:#x}: {} heads are UObjects in the object array{}",
            offset, score,
            children_candidates.size() > 1
                ? std::format(" (lowest of {} candidates)", children_candidates.size()) : ""));
        break;
    }

    for (const auto& [offset, score] : property_candidates) {
        if (score < minimum_heads) continue;
        layout.child_properties = offset;
        layout.evidence.push_back(std::format(
            "ChildProperties at +{:#x}: {} heads are FFields outside the object array{}",
            offset, score,
            property_candidates.size() > 1
                ? std::format(" (lowest of {} candidates; higher ones are the PropertyLink "
                              "chain)", property_candidates.size()) : ""));
        break;
    }

    // --- scoring ----------------------------------------------------------------------
    int resolved = 0;
    for (const int offset : {layout.super_struct, layout.children, layout.child_properties,
                             layout.properties_size, layout.min_alignment})
        if (offset >= 0) ++resolved;

    layout.confidence = 0.4f * static_cast<float>(resolved) / 5.0f;

    // End-to-end check. UObject itself has no super, and its size must match the object
    // layout derived independently earlier. Two separate derivations agreeing is far
    // stronger evidence than either alone.
    if (!IsNull(layout.object_class) && layout.properties_size >= 0) {
        const auto root_super = core::ReadOr<Address>(
            memory, layout.object_class + layout.super_struct);
        const std::int32_t root_size = core::ReadOr<std::int32_t>(
            memory, layout.object_class + layout.properties_size);

        const bool no_super = IsNull(root_super);

        // The floor, not the figure: those members are there, so a UObject is at least that
        // big. A build that appends to UObject is above it and still correct.
        const int  floor     = object_layout.outer_offset + 8;
        const bool size_fits = root_size == layout.object_size && root_size >= floor;

        layout.evidence.push_back(std::format(
            "UObject: super {}, PropertiesSize {} (its members account for {})",
            no_super ? "null" : "NOT null", root_size, floor));

        if (no_super && size_fits) {
            // A shade lower for a fork, since it rests on one assumption fewer than a stock
            // build does -- enough to notice in the header, not enough to distrust.
            const float earned = layout.extends_uobject ? 0.45f : 0.55f;
            layout.confidence = std::min(0.98f, layout.confidence + earned);
        } else {
            core::LogWarn("UObject sanity check failed: super {}, size {} (members account "
                          "for {})", no_super ? "null" : "non-null", root_size, floor);
            layout.evidence.push_back("end-to-end check FAILED on /Script/CoreUObject.Object");
            layout.confidence *= 0.3f;
        }
    }

    // "+{:#x}" on an unresolved -1 prints "+-0x1", which reads like an offset rather than
    // like a failure.
    const auto at = [](int offset) {
        return offset < 0 ? std::string("unresolved") : std::format("+{:#x}", offset);
    };
    core::LogInfo("UStruct layout: super {}, children {}, childprops {}, size {}, align {}",
                  at(layout.super_struct), at(layout.children), at(layout.child_properties),
                  at(layout.properties_size), at(layout.min_alignment));
    return layout;
}

Address GetSuperStruct(core::IMemorySource& memory, const UStructLayout& layout,
                       Address structure) {
    if (IsNull(structure) || layout.super_struct < 0) return {};
    return core::ReadOr<Address>(memory, structure + layout.super_struct);
}

std::int32_t GetMinAlignment(core::IMemorySource& memory, const UStructLayout& layout,
                             Address structure) {
    if (IsNull(structure) || layout.min_alignment < 0) return 0;
    if (layout.min_alignment_is_u16)
        return core::ReadOr<std::uint16_t>(memory, structure + layout.min_alignment);
    return core::ReadOr<std::int32_t>(memory, structure + layout.min_alignment);
}

std::int32_t GetPropertiesSize(core::IMemorySource& memory, const UStructLayout& layout,
                               Address structure) {
    if (IsNull(structure) || layout.properties_size < 0) return 0;
    return core::ReadOr<std::int32_t>(memory, structure + layout.properties_size);
}

Address FindObjectByPath(core::IMemorySource& memory, const ObjectArrayInfo& array,
                         const UObjectLayout& layout, const NamePoolInfo& pool,
                         std::string_view path) {
    for (std::int32_t index = 0; index < array.num_elements; ++index) {
        const auto object = ObjectAt(memory, array, index);
        if (IsNull(object)) continue;
        if (GetObjectPathName(memory, layout, pool, object) == path) return object;
    }
    return {};
}

} // namespace zircon::engine
