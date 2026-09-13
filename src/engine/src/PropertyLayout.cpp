#include "engine/PropertyLayout.h"
#include "core/Log.h"

#include <algorithm>
#include <format>
#include <set>

namespace zircon::engine {
namespace {

using core::Address;
using core::IsNull;
using core::Raw;

constexpr int kMaxFieldOffset    = 0x70;
constexpr int kStructSampleTarget = 400;
constexpr int kMaxChainLength     = 4096;

bool Readable(core::IMemorySource& memory, Address addr, std::size_t size = 8) {
    if (IsNull(addr) || Raw(addr) < 0x10000 || Raw(addr) >= 0x7FFFFFFFFFFFull) return false;
    std::uint8_t probe[64];
    const std::size_t want = std::min(size, sizeof(probe));
    return memory.Read(addr, probe, want) == want;
}

// An FFieldClass begins with the type's FName, so a candidate ClassPrivate is confirmed by
// that name reading as something like "ObjectProperty". Every concrete FProperty subclass
// is named that way, which makes the suffix a far tighter filter than "resolves to some
// printable name".
bool LooksLikePropertyTypeName(const std::string& name) {
    constexpr std::string_view kSuffix = "Property";
    return name.size() > kSuffix.size() &&
           name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
}

struct StructSample {
    Address structure{};
    Address head{};
    std::int32_t size{};
};

} // namespace

// Numeric members, shared by both property systems.
//
// All of it works from a list of properties plus the size of the struct each belongs to.
// Neither fact cares whether the property is an FField or a UObject. The two eras differ
// only in how the list gets found, so this is written once.
struct PropertySample {
    Address      field{};
    std::int32_t owner_size{};
};

void DeriveNumericMembers(core::IMemorySource& memory, const ObjectArrayInfo& array,
                          const NamePoolInfo& pool, const UObjectLayout& object_layout,
                          const UStructLayout& struct_layout,
                          const std::vector<StructSample>& samples,
                          const std::vector<PropertySample>& properties,
                          int first_numeric, FPropertyLayout& layout) {
    // --- ArrayDim, ElementSize, Offset_Internal ---------------------------------------
    // Three small int32s sitting close together, which is exactly the shape that produced
    // wrong answers in earlier phases. So pin each by a different constraint, never by
    // position:
    //
    //   ArrayDim        is 1 for the overwhelming majority of properties
    //   ElementSize     is positive, and a property never overruns its owning struct
    //   Offset_Internal is inside the owner, and at least one property sits at 0
    //
    // That last clause earns its keep. Without it ElementSize and Offset_Internal are hard
    // to tell apart on any struct whose first member isn't at zero.
    {
        int best_dim = -1, best_dim_score = 0;
        for (int offset = first_numeric; offset <= kMaxFieldOffset; offset += 4) {
            int ones = 0, sane = 0;
            for (const auto& property : properties) {
                const auto value = core::ReadOr<std::int32_t>(memory, property.field + offset);
                if (value == 1) ++ones;
                if (value >= 1 && value <= 4096) ++sane;
            }
            const int total = static_cast<int>(properties.size());
            if (sane >= total * 95 / 100 && ones >= total * 85 / 100 && ones > best_dim_score) {
                best_dim_score = ones;
                best_dim       = offset;
            }
        }
        if (best_dim >= 0) {
            layout.array_dim = best_dim;
            layout.evidence.push_back(std::format(
                "FProperty::ArrayDim at +{:#x}: {}/{} properties hold exactly 1",
                best_dim, best_dim_score, properties.size()));
        }
    }

    {
        int best_size = -1, best_size_score = 0;
        for (int offset = first_numeric; offset <= kMaxFieldOffset; offset += 4) {
            if (offset == layout.array_dim) continue;

            int plausible = 0;
            for (const auto& property : properties) {
                const auto value = core::ReadOr<std::int32_t>(memory, property.field + offset);
                if (value > 0 && value <= property.owner_size) ++plausible;
            }
            if (plausible > best_size_score) {
                best_size_score = plausible;
                best_size       = offset;
            }
        }
        if (best_size >= 0 &&
            best_size_score >= static_cast<int>(properties.size()) * 9 / 10) {
            layout.element_size = best_size;
            layout.evidence.push_back(std::format(
                "FProperty::ElementSize at +{:#x}: {}/{} fit inside their owning struct",
                best_size, best_size_score, properties.size()));
        }
    }

    // PropertyFlags follows ElementSize. Done before Offset_Internal so that search can
    // exclude the eight bytes it takes up. Still confirmed rather than assumed — flags vary
    // widely between properties and are never uniformly zero.
    if (layout.element_size >= 0) {
        const int candidate = layout.element_size + 4;
        std::set<std::uint64_t> distinct;
        for (const auto& property : properties)
            distinct.insert(core::ReadOr<std::uint64_t>(memory, property.field + candidate));

        if (distinct.size() > 4) {
            layout.property_flags = candidate;
            layout.evidence.push_back(std::format(
                "FProperty::PropertyFlags at +{:#x}: {} distinct flag words",
                candidate, distinct.size()));
        }
    }

    {
        // "Lies inside the owning struct" is NOT sufficient, and it's an easy mistake to
        // make. A field that's always zero satisfies 0 <= v < size for every property and
        // scores perfectly, giving you a layout where every member sits at offset 0.
        //
        // What actually characterises Offset_Internal is that one struct's properties
        // *tile* it: largely distinct offsets, with the last ending inside the struct
        // rather than short of it or past it. Bitfields legitimately share an offset, so
        // demand distinctness of most members and no more.
        //
        // Per-struct lists get rebuilt here because the constraint is about a struct's
        // properties as a set, and never about one property on its own.
        int    best_offset  = -1;
        double best_score   = 0.0;
        double second_score = 0.0;

        for (int offset = first_numeric; offset <= kMaxFieldOffset; offset += 4) {
            if (offset == layout.array_dim || offset == layout.element_size) continue;
            if (layout.property_flags >= 0 &&
                offset >= layout.property_flags && offset < layout.property_flags + 8)
                continue;

            int considered = 0, tiled = 0;

            for (const auto& sample : samples) {
                const auto fields = GetChildProperties(memory, struct_layout, layout,
                                                       sample.structure);
                if (fields.size() < 2) continue;
                ++considered;

                std::set<std::int32_t> distinct;
                std::int32_t highest_end = 0;
                bool sane = true;

                for (const auto field : fields) {
                    const auto at = core::ReadOr<std::int32_t>(memory, field + offset);
                    const auto element =
                        core::ReadOr<std::int32_t>(memory, field + layout.element_size);
                    const auto dim = layout.array_dim >= 0
                        ? core::ReadOr<std::int32_t>(memory, field + layout.array_dim) : 1;

                    if (at < 0 || element <= 0 || dim <= 0) { sane = false; break; }

                    const std::int64_t end =
                        static_cast<std::int64_t>(at) + static_cast<std::int64_t>(element) * dim;
                    if (end > sample.size) { sane = false; break; }

                    distinct.insert(at);
                    highest_end = std::max<std::int32_t>(highest_end,
                                                         static_cast<std::int32_t>(end));
                }
                if (!sane) continue;

                // Most members at distinct offsets, struct actually filled.
                if (distinct.size() * 2 < fields.size()) continue;
                if (highest_end * 2 < sample.size) continue;
                ++tiled;
            }

            if (considered == 0) continue;
            const double score = static_cast<double>(tiled) / considered;

            // When this search fails there's otherwise nothing to look at. The answer is one
            // rejected number and the reason lives in the spread of the runners-up.
            if (score > 0.25)
                core::LogDebug("  Offset_Internal candidate +{:#x}: {}/{} structs tiled ({:.0f}%)",
                               offset, tiled, considered, score * 100.0);

            if (score > best_score) {
                second_score = best_score;
                best_score   = score;
                best_offset  = offset;
            } else if (score > second_score) {
                second_score = score;
            }
        }

        // Judged by separation.
        //
        // A flat 90% rejected UE 5.0. The right offset scored 238/265 = 89.8% and was the
        // only candidate above 25%, so the evidence was overwhelming and the threshold said
        // no anyway. How cleanly a struct tiles depends on how odd its types are, and that
        // varies by game. The *gap* to the runner-up doesn't.
        //
        // Both halves earn their keep: the floor stops a field that only wins because every
        // candidate is bad, the gap stops a field that scores well next to an equally good
        // rival. That second case is the genuinely ambiguous one and the one worth refusing.
        const bool decisive = second_score < best_score * 0.6;

        if (best_offset >= 0 && best_score >= 0.75 && decisive) {
            layout.offset_internal = best_offset;
            layout.evidence.push_back(std::format(
                "FProperty::Offset_Internal at +{:#x}: {:.0f}% of sampled structs are "
                "tiled by their own properties at distinct offsets, against {:.0f}% for the "
                "next best candidate",
                best_offset, best_score * 100.0, second_score * 100.0));
        } else if (best_offset >= 0) {
            core::LogWarn("Offset_Internal not settled: best +{:#x} at {:.0f}%, runner-up "
                          "{:.0f}%", best_offset, best_score * 100.0, second_score * 100.0);
        }
    }

    // --- end-to-end check -------------------------------------------------------------
    // FVector's layout is fixed by the engine: three components from 0, one element wide
    // each, filling the struct exactly. Wrong numeric offsets can't pass this.
    layout.confidence = 0.4f;

    const auto vector_struct = FindObjectByPath(memory, array, object_layout, pool,
                                                "/Script/CoreUObject.Vector");
    if (!IsNull(vector_struct) && layout.Valid()) {
        const auto fields = GetChildProperties(memory, struct_layout, layout, vector_struct);
        const std::int32_t vector_size = GetPropertiesSize(memory, struct_layout, vector_struct);

        bool ok = fields.size() == 3 && vector_size > 0;
        std::string detail;
        if (ok) {
            std::int32_t expected = 0;
            for (const auto field : fields) {
                const std::string name = GetFieldName(memory, layout, pool, field);
                const std::int32_t at   = GetPropertyOffset(memory, layout, field);
                const std::int32_t size = GetElementSize(memory, layout, field);
                detail += std::format("{}@{}({}) ", name, at, size);
                if (at != expected || size <= 0) ok = false;
                expected += size;
            }
            if (expected != vector_size) ok = false;
        }

        layout.evidence.push_back(std::format(
            "/Script/CoreUObject.Vector: {} components [{}] filling {} bytes",
            fields.size(), detail, vector_size));

        if (ok) {
            layout.confidence = 0.96f;
        } else {
            core::LogWarn("FVector layout check failed; property offsets are probably wrong");
            layout.evidence.push_back("end-to-end check FAILED on /Script/CoreUObject.Vector");
            layout.confidence = 0.2f;
        }
    }

}


// The 4.24-and-earlier property walk.
//
// A UProperty is a UObject, so most of the layout comes free: class pointer, name and
// identity all live where every UObject keeps them. That leaves UField::Next, plus the same
// numeric members the FField path derives.
//
// The one genuinely new problem: UStruct::Children isn't a property list. It's a UField
// list carrying functions and enums too, so walking it and believing what turns up would
// hand the numeric derivation a pile of UFunctions. Ancestry picks the properties out
// instead — an entry is a property exactly when its class chain reaches
// /Script/CoreUObject.Property. That object is found by name, so it's an anchor with a
// known-correct value, not a guess about what a name looks like.
FPropertyLayout DeriveUPropertyLayout(core::IMemorySource& memory,
                                      const ObjectArrayInfo& array,
                                      const NamePoolInfo& pool,
                                      const UObjectLayout& object_layout,
                                      const UStructLayout& struct_layout) {
    FPropertyLayout layout;
    layout.uproperty = true;

    const auto property_class =
        FindObjectByPath(memory, array, object_layout, pool, "/Script/CoreUObject.Property");
    if (IsNull(property_class)) {
        core::LogWarn("no /Script/CoreUObject.Property; cannot identify UProperty objects");
        return layout;
    }

    // Already known from the UObject layout; no point searching twice.
    layout.class_private     = object_layout.class_offset;
    layout.name              = object_layout.name_offset;
    layout.class_name_offset = object_layout.name_offset;

    // An object is in the array when the slot it claims holds it back.
    auto in_array = [&](Address candidate) {
        if (IsNull(candidate)) return false;
        const auto index = core::ReadOr<std::int32_t>(memory, candidate + array.index_offset);
        if (index < 0 || index >= array.num_elements) return false;
        return Raw(ObjectAt(memory, array, index)) == Raw(candidate);
    };

    // Collect every property class once up front. A super-chain walk per entry per struct
    // recomputes the same answer tens of thousands of times, and the accessors need a test
    // that works without the object array regardless.
    for (std::int32_t index = 0; index < array.num_elements; ++index) {
        const auto object = ObjectAt(memory, array, index);
        if (IsNull(object)) continue;
        if (GetClassName(memory, object_layout, pool, object) != "Class") continue;

        auto current = object;
        for (std::size_t guard = 0; !IsNull(current) && guard < 64; ++guard) {
            if (Raw(current) == Raw(property_class)) {
                layout.property_classes.push_back(Raw(object));
                break;
            }
            current = GetSuperStruct(memory, struct_layout, current);
        }
    }
    std::sort(layout.property_classes.begin(), layout.property_classes.end());

    if (layout.property_classes.size() < 8) {
        core::LogWarn("only {} property classes found below /Script/CoreUObject.Property",
                      layout.property_classes.size());
        return layout;
    }
    core::LogDebug("{} UProperty subclasses", layout.property_classes.size());

    auto is_property = [&](Address object) {
        if (!in_array(object)) return false;
        const auto klass = core::ReadOr<Address>(memory, object + layout.class_private);
        return std::binary_search(layout.property_classes.begin(),
                                  layout.property_classes.end(), Raw(klass));
    };

    // Structs that declare something. ScriptStructs are the valuable ones here for the same
    // reason as in the FField path: members starting at zero are what pins Offset_Internal
    // apart from ElementSize.
    std::vector<StructSample> samples;
    for (std::int32_t index = 0;
         index < array.num_elements && static_cast<int>(samples.size()) < kStructSampleTarget;
         ++index) {
        const auto object = ObjectAt(memory, array, index);
        if (IsNull(object)) continue;

        const std::string kind = GetClassName(memory, object_layout, pool, object);
        if (kind != "Class" && kind != "ScriptStruct") continue;

        const auto head = core::ReadOr<Address>(memory, object + struct_layout.children);
        if (!in_array(head)) continue;

        StructSample sample;
        sample.structure = object;
        sample.head      = head;
        sample.size      = GetPropertiesSize(memory, struct_layout, object);
        if (sample.size <= 0) continue;
        samples.push_back(sample);
    }

    if (samples.size() < 16) {
        core::LogWarn("only {} structs with children sampled; too few to derive the "
                      "UProperty layout", samples.size());
        return layout;
    }

    // --- UField::Next -----------------------------------------------------------------
    // Every link lands on another object in the array, and the chain terminates. A pointer
    // that loops or leaves the array isn't a field list. Unlike the FField case we don't
    // demand the chain be all properties: mixing in functions is what Children does.
    for (int offset = object_layout.outer_offset + 8;
         offset <= kMaxFieldOffset && layout.next < 0; offset += 8) {
        if (offset == struct_layout.super_struct || offset == struct_layout.children) continue;

        int good = 0, saw_property = 0;
        for (const auto& sample : samples) {
            Address current = sample.head;
            std::set<std::uint64_t> seen;
            bool sane = true;
            int length = 0;

            while (length < kMaxChainLength) {
                if (is_property(current)) ++saw_property;
                const auto next = core::ReadOr<Address>(memory, current + offset);
                if (IsNull(next)) break;
                if (!in_array(next))                        { sane = false; break; }
                if (!seen.insert(Raw(next)).second)         { sane = false; break; }
                current = next;
                ++length;
            }
            if (sane) ++good;
        }

        // Clean termination isn't enough by itself. An always-null slot terminates
        // perfectly and walks nothing, so the chains have to actually reach properties.
        if (good >= static_cast<int>(samples.size()) * 9 / 10 && saw_property >= 32) {
            layout.next = offset;
            layout.evidence.push_back(std::format(
                "UField::Next at +{:#x}: {}/{} Children chains stay in the object array and "
                "terminate, reaching {} properties", offset, good, samples.size(),
                saw_property));
        }
    }

    if (layout.next < 0) {
        core::LogWarn("could not identify UField::Next");
        return layout;
    }

    // Collect the properties themselves, skipping the functions and enums that share the
    // list. Their owning struct's size travels with them, which is what makes
    // Offset_Internal checkable.
    std::vector<PropertySample> properties;
    for (const auto& sample : samples) {
        Address current = sample.head;
        std::set<std::uint64_t> seen;
        while (!IsNull(current) && seen.insert(Raw(current)).second &&
               properties.size() < 20000) {
            if (is_property(current)) properties.push_back(PropertySample{current, sample.size});
            current = core::ReadOr<Address>(memory, current + layout.next);
        }
    }

    core::LogDebug("collected {} UProperties from {} structs", properties.size(),
                   samples.size());

    if (properties.size() < 64) {
        core::LogWarn("only {} UProperties found; too few to derive their layout",
                      properties.size());
        return layout;
    }

    // UProperty's own members start past UField::Next.
    DeriveNumericMembers(memory, array, pool, object_layout, struct_layout, samples,
                         properties, layout.next + 8, layout);

    core::LogInfo("UProperty layout: class +{:#x}, next +{:#x}, name +{:#x}, dim +{:#x}, "
                  "size +{:#x}, flags +{:#x}, offset +{:#x}",
                  layout.class_private, layout.next, layout.name, layout.array_dim,
                  layout.element_size, layout.property_flags, layout.offset_internal);
    return layout;
}

FPropertyLayout DerivePropertyLayout(core::IMemorySource& memory,
                                     const ObjectArrayInfo& array,
                                     const NamePoolInfo& pool,
                                     const UObjectLayout& object_layout,
                                     const UStructLayout& struct_layout) {
    FPropertyLayout layout;

    if (!struct_layout.Valid()) {
        core::LogWarn("UStruct layout incomplete; cannot walk properties");
        return layout;
    }

    // No ChildProperties means no FFields, so 4.24 or earlier, with the properties being
    // UObjects hanging off Children. The absence itself is the signal — a fact about the
    // target, not an inference from a version string. Which matters, because the licensee
    // builds that most need this tool are exactly the ones with no version string.
    if (struct_layout.child_properties < 0) {
        if (struct_layout.children < 0) {
            core::LogWarn("UStruct has neither ChildProperties nor Children; cannot walk "
                          "properties");
            return layout;
        }
        return DeriveUPropertyLayout(memory, array, pool, object_layout, struct_layout);
    }

    // Structs that actually declare properties. ScriptStructs are the most useful samples:
    // their properties start at offset 0, which is what anchors Offset_Internal.
    std::vector<StructSample> samples;
    for (std::int32_t index = 0;
         index < array.num_elements && static_cast<int>(samples.size()) < kStructSampleTarget;
         ++index) {
        const auto object = ObjectAt(memory, array, index);
        if (IsNull(object)) continue;

        const std::string kind = GetClassName(memory, object_layout, pool, object);
        if (kind != "Class" && kind != "ScriptStruct") continue;

        const auto head = core::ReadOr<Address>(memory, object + struct_layout.child_properties);
        if (!Readable(memory, head, 32)) continue;

        StructSample sample;
        sample.structure = object;
        sample.head      = head;
        sample.size      = GetPropertiesSize(memory, struct_layout, object);
        if (sample.size <= 0) continue;
        samples.push_back(sample);
    }

    if (samples.size() < 16) {
        core::LogWarn("only {} structs with properties sampled; too few to derive the "
                      "FProperty layout", samples.size());
        return layout;
    }

    // --- FFieldClass pointer ----------------------------------------------------------
    for (int offset = 0; offset <= kMaxFieldOffset && layout.class_private < 0; offset += 8) {
        int hits = 0;
        for (const auto& sample : samples) {
            const auto field_class = core::ReadOr<Address>(memory, sample.head + offset);
            if (!Readable(memory, field_class, 8)) continue;

            const auto id = core::ReadOr<std::uint32_t>(memory, field_class);
            if (LooksLikePropertyTypeName(ResolveName(memory, pool, id))) ++hits;
        }

        if (hits >= static_cast<int>(samples.size()) * 3 / 4) {
            layout.class_private = offset;
            layout.evidence.push_back(std::format(
                "FField::ClassPrivate at +{:#x}: {}/{} heads name a *Property type",
                offset, hits, samples.size()));
        }
    }

    if (layout.class_private < 0) {
        core::LogWarn("could not identify FField::ClassPrivate");
        return layout;
    }

    auto type_name_of = [&](Address field) {
        const auto field_class = core::ReadOr<Address>(memory, field + layout.class_private);
        if (IsNull(field_class)) return std::string{};
        return ResolveName(memory, pool, core::ReadOr<std::uint32_t>(memory, field_class));
    };

    // --- FField::NamePrivate ----------------------------------------------------------
    // Same discipline as UObject. Demand diversity, not mere resolvability, so a
    // slot that happens to alias a constant pointer half can't win.
    {
        int best_offset = -1;
        std::size_t best_distinct = 0;

        for (int offset = 8; offset <= kMaxFieldOffset; offset += 4) {
            if (offset == layout.class_private) continue;

            std::set<std::string> distinct;
            int hits = 0;
            for (const auto& sample : samples) {
                const auto id = core::ReadOr<std::uint32_t>(memory, sample.head + offset);
                const std::string name = ResolveName(memory, pool, id);
                if (name.empty()) continue;
                ++hits;
                distinct.insert(name);
            }

            if (hits < static_cast<int>(samples.size()) * 3 / 4) continue;
            if (distinct.size() > best_distinct) {
                best_distinct = distinct.size();
                best_offset   = offset;
            }
        }

        if (best_offset >= 0 && best_distinct >= 8) {
            layout.name = best_offset;
            layout.evidence.push_back(std::format(
                "FField::NamePrivate at +{:#x}: {} distinct names across {} heads",
                best_offset, best_distinct, samples.size()));
        }
    }

    // --- FField::Next -----------------------------------------------------------------
    // A property's successor is another property, so confirm the candidate by having the
    // target name a *Property type through the ClassPrivate offset just fixed above. Plus
    // termination: a pointer that loops is not a list head.
    for (int offset = 8; offset <= kMaxFieldOffset && layout.next < 0; offset += 8) {
        if (offset == layout.class_private) continue;

        int good_chains = 0;
        for (const auto& sample : samples) {
            Address current = sample.head;
            std::set<std::uint64_t> seen;
            bool sane = true;
            int length = 0;

            while (length < kMaxChainLength) {
                const auto next = core::ReadOr<Address>(memory, current + offset);
                if (IsNull(next)) break;
                if (!Readable(memory, next, 32))            { sane = false; break; }
                if (!seen.insert(Raw(next)).second)         { sane = false; break; }
                if (!LooksLikePropertyTypeName(type_name_of(next))) { sane = false; break; }
                current = next;
                ++length;
            }

            if (sane) ++good_chains;
        }

        if (good_chains >= static_cast<int>(samples.size()) * 9 / 10) {
            layout.next = offset;
            layout.evidence.push_back(std::format(
                "FField::Next at +{:#x}: {}/{} chains walk only *Property types and "
                "terminate", offset, good_chains, samples.size()));
        }
    }

    if (layout.next < 0) {
        core::LogWarn("could not identify FField::Next");
        return layout;
    }

    // The owning struct travels with each property. That pairing is what makes the numeric
    // fields checkable at all.
    std::vector<PropertySample> properties;

    for (const auto& sample : samples) {
        Address current = sample.head;
        std::set<std::uint64_t> seen;
        while (!IsNull(current) && seen.insert(Raw(current)).second &&
               properties.size() < 20000) {
            properties.push_back(PropertySample{current, sample.size});
            current = core::ReadOr<Address>(memory, current + layout.next);
        }
    }

    core::LogDebug("collected {} properties from {} structs", properties.size(),
                   samples.size());

    // Numeric members begin past the last pointer-sized header field, which lands in a
    // different place in each property system.
    const int first_numeric = std::max(layout.next, layout.name) + 8;

    DeriveNumericMembers(memory, array, pool, object_layout, struct_layout, samples,
                         properties, first_numeric, layout);

    core::LogInfo("FProperty layout: class +{:#x}, next +{:#x}, name +{:#x}, dim +{:#x}, "
                  "size +{:#x}, flags +{:#x}, offset +{:#x}",
                  layout.class_private, layout.next, layout.name, layout.array_dim,
                  layout.element_size, layout.property_flags, layout.offset_internal);
    return layout;
}

// --- accessors ----------------------------------------------------------------------

std::string GetFieldName(core::IMemorySource& memory, const FPropertyLayout& layout,
                         const NamePoolInfo& pool, Address field) {
    if (IsNull(field) || layout.name < 0) return {};
    const auto id     = core::ReadOr<std::uint32_t>(memory, field + layout.name);
    const auto number = core::ReadOr<std::int32_t>(memory, field + layout.name + 4);
    return ResolveFName(memory, pool, id, number);
}

std::string GetPropertyTypeName(core::IMemorySource& memory, const FPropertyLayout& layout,
                                const NamePoolInfo& pool, Address field) {
    if (IsNull(field) || layout.class_private < 0) return {};
    const auto field_class = core::ReadOr<Address>(memory, field + layout.class_private);
    if (IsNull(field_class)) return {};

    // FFieldClass starts with its FName. A UClass keeps it where every UObject does.
    return ResolveName(memory, pool,
                       core::ReadOr<std::uint32_t>(memory,
                                                   field_class + layout.class_name_offset));
}

Address GetNextField(core::IMemorySource& memory, const FPropertyLayout& layout,
                     Address field) {
    if (IsNull(field) || layout.next < 0) return {};
    return core::ReadOr<Address>(memory, field + layout.next);
}

std::int32_t GetPropertyOffset(core::IMemorySource& memory, const FPropertyLayout& layout,
                               Address field) {
    if (IsNull(field) || layout.offset_internal < 0) return 0;
    return core::ReadOr<std::int32_t>(memory, field + layout.offset_internal);
}

std::int32_t GetElementSize(core::IMemorySource& memory, const FPropertyLayout& layout,
                            Address field) {
    if (IsNull(field) || layout.element_size < 0) return 0;
    return core::ReadOr<std::int32_t>(memory, field + layout.element_size);
}

std::int32_t GetArrayDim(core::IMemorySource& memory, const FPropertyLayout& layout,
                         Address field) {
    if (IsNull(field) || layout.array_dim < 0) return 1;
    return core::ReadOr<std::int32_t>(memory, field + layout.array_dim);
}

std::uint64_t GetPropertyFlags(core::IMemorySource& memory, const FPropertyLayout& layout,
                               Address field) {
    if (IsNull(field) || layout.property_flags < 0) return 0;
    return core::ReadOr<std::uint64_t>(memory, field + layout.property_flags);
}

std::vector<Address> GetChildProperties(core::IMemorySource& memory,
                                        const UStructLayout& struct_layout,
                                        const FPropertyLayout& property_layout,
                                        Address structure) {
    std::vector<Address> fields;
    if (IsNull(structure) || property_layout.next < 0) return fields;

    // Before 4.25 the list is Children, which carries functions and enums too, so every
    // entry needs checking. The check is membership against the property classes collected
    // during derivation — read an entry's class pointer, ask whether it's one of them. That
    // needs neither the object array nor the name pool, which is how this accessor keeps
    // one signature across both eras.
    const int head_offset = property_layout.uproperty ? struct_layout.children
                                                      : struct_layout.child_properties;
    if (head_offset < 0) return fields;

    const auto& property_classes = property_layout.property_classes;

    auto is_property = [&](Address object) {
        if (!property_layout.uproperty) return true;
        if (property_layout.class_private < 0) return false;

        const auto klass = core::ReadOr<Address>(memory, object + property_layout.class_private);
        return std::binary_search(property_classes.begin(), property_classes.end(), Raw(klass));
    };

    std::set<std::uint64_t> seen;
    Address current = core::ReadOr<Address>(memory, structure + head_offset);

    while (!IsNull(current) && seen.insert(Raw(current)).second &&
           fields.size() < kMaxChainLength) {
        if (is_property(current)) fields.push_back(current);
        current = core::ReadOr<Address>(memory, current + property_layout.next);
    }
    return fields;
}

} // namespace zircon::engine
