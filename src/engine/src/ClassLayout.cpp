#include "engine/ClassLayout.h"
#include "core/Log.h"

#include <format>

namespace zircon::engine {
namespace {

using core::Address;
using core::IsNull;
using core::Raw;

constexpr int kMaxProbe     = 0x400;   // UClass is large; the CDO sits well past UStruct
constexpr int kSampleTarget = 300;

bool IsArrayObject(core::IMemorySource& memory, const ObjectArrayInfo& array, Address candidate) {
    if (IsNull(candidate)) return false;
    std::int32_t index{};
    if (!core::ReadInto(memory, candidate + array.index_offset, index)) return false;
    if (index < 0 || index >= array.num_elements) return false;
    return Raw(ObjectAt(memory, array, index)) == Raw(candidate);
}

// --- UClass::Interfaces ----------------------------------------------------------------
//
// A TArray<FImplementedInterface>. What makes it findable is not its shape -- UClass holds
// several TArrays -- but what its elements point at: every element's first pointer is a
// UClass that inherits from /Script/CoreUObject.Interface. ClassReps, the other array of
// pointer-plus-int32 in UClass, holds FProperty pointers, which are not array objects at
// all, so the two do not compete.
//
// The anchor with a known-correct value is the Interface class itself, found by name
// through the pool. Without it this would be another pure-consistency test, and those have
// been wrong six times in this project.
void DeriveInterfaces(core::IMemorySource& memory, const ObjectArrayInfo& array,
                      const NamePoolInfo& pool, const UObjectLayout& object_layout,
                      const UStructLayout& struct_layout,
                      const std::vector<Address>& classes, UClassLayout& layout) {
    const auto interface_class =
        FindObjectByPath(memory, array, object_layout, pool, "/Script/CoreUObject.Interface");
    if (IsNull(interface_class)) {
        core::LogDebug("no /Script/CoreUObject.Interface; interface lists unavailable");
        return;
    }

    auto inherits_interface = [&](Address klass) {
        std::size_t guard = 0;
        for (auto current = klass; !IsNull(current) && guard < 64; ++guard) {
            if (Raw(current) == Raw(interface_class)) return true;
            current = GetSuperStruct(memory, struct_layout, current);
        }
        return false;
    };

    // 16 first, since that's what { ptr, int32, bool } pads to on 64-bit. Confirmed from
    // the data all the same.
    constexpr int kStrides[] = {16, 24, 12};

    int best_offset = -1, best_stride = 0, best_classes = 0, best_entries = 0;

    for (int offset = struct_layout.properties_size; offset <= kMaxProbe; offset += 8) {
        if (offset == layout.class_default_object) continue;

        for (const int stride : kStrides) {
            int with_entries = 0, entries = 0, contradictions = 0;

            for (const auto klass : classes) {
                const auto data = core::ReadOr<Address>(memory, klass + offset);
                const auto count = core::ReadOr<std::int32_t>(memory, klass + offset + 8);
                const auto max   = core::ReadOr<std::int32_t>(memory, klass + offset + 12);

                if (count == 0 && IsNull(data)) continue;          // no interfaces: fine
                if (count <= 0 || count > 256 || max < count) { ++contradictions; break; }
                if (IsNull(data)) { ++contradictions; break; }

                bool all_interfaces = true;
                for (std::int32_t i = 0; i < count; ++i) {
                    const auto entry = core::ReadOr<Address>(
                        memory, data + static_cast<std::uint64_t>(i) * stride);

                    if (!IsArrayObject(memory, array, entry) ||
                        ClassifyObject(memory, object_layout, struct_layout, pool, entry) !=
                            ObjectKind::Class ||
                        !inherits_interface(entry)) {
                        all_interfaces = false;
                        break;
                    }
                }

                if (!all_interfaces) { ++contradictions; break; }
                ++with_entries;
                entries += count;
            }

            // One class whose array doesn't hold interfaces disqualifies the offset. A
            // field that's right for most classes and wrong for one isn't the field, it's
            // a coincidence with a bad case.
            if (contradictions > 0 || with_entries < 8) continue;

            if (with_entries > best_classes) {
                best_offset  = offset;
                best_stride  = stride;
                best_classes = with_entries;
                best_entries = entries;
            }
        }
    }

    if (best_offset < 0) {
        core::LogDebug("UClass::Interfaces not identified; interface lists will be empty");
        return;
    }

    layout.interfaces        = best_offset;
    layout.interface_stride  = best_stride;
    layout.evidence.push_back(std::format(
        "UClass::Interfaces at +{:#x} (stride {}): {} of {} sampled classes list {} entries, "
        "every one a class inheriting /Script/CoreUObject.Interface",
        best_offset, best_stride, best_classes, classes.size(), best_entries));

    core::LogInfo("UClass layout: Interfaces +{:#x}, stride {} ({} classes, {} entries)",
                  best_offset, best_stride, best_classes, best_entries);
}

} // namespace

UClassLayout DeriveClassLayout(core::IMemorySource& memory,
                               const ObjectArrayInfo& array,
                               const NamePoolInfo& pool,
                               const UObjectLayout& object_layout,
                               const UStructLayout& struct_layout) {
    UClassLayout layout;
    if (!object_layout.Valid() || !struct_layout.Valid()) return layout;

    // Front of the array, where the engine's own classes live. Those are guaranteed fully
    // constructed, CDO included.
    std::vector<Address> classes;
    for (std::int32_t index = 0;
         index < array.num_elements && static_cast<int>(classes.size()) < kSampleTarget;
         ++index) {
        const auto object = ObjectAt(memory, array, index);
        if (IsNull(object)) continue;
        if (ClassifyObject(memory, object_layout, struct_layout, pool, object) == ObjectKind::Class)
            classes.push_back(object);
    }

    if (classes.size() < 32) {
        core::LogWarn("only {} classes sampled; cannot derive the class layout", classes.size());
        return layout;
    }

    // Scored separately so the evidence line can say which tests a candidate actually
    // passed. An offset that closes the class loop but never yields a "Default__" name is
    // not the CDO pointer, however consistent it looks.
    int  best_offset   = -1;
    int  best_loops    = 0;
    int  best_prefixed = 0;

    for (int offset = struct_layout.properties_size; offset <= kMaxProbe; offset += 8) {
        if (offset == struct_layout.super_struct || offset == struct_layout.children ||
            offset == struct_layout.child_properties)
            continue;

        int loops = 0;      // points at an array object whose class is this very class
        int prefixed = 0;   // ...and that object is named Default__<ClassName>
        int nonnull = 0;

        for (const auto klass : classes) {
            const auto candidate = core::ReadOr<Address>(memory, klass + offset);
            if (IsNull(candidate)) continue;
            ++nonnull;

            if (!IsArrayObject(memory, array, candidate)) continue;
            if (Raw(GetObjectClass(memory, object_layout, candidate)) != Raw(klass)) continue;
            ++loops;

            const std::string name = GetObjectName(memory, object_layout, pool, candidate);
            if (name.rfind("Default__", 0) == 0) ++prefixed;
        }

        // A real CDO pointer is non-null for almost every engine class, and every non-null
        // one closes the loop. Demanding both rules out a field that merely happens to hold
        // a self-referential pointer now and then.
        if (nonnull < static_cast<int>(classes.size()) * 3 / 4) continue;
        if (loops != nonnull) continue;
        if (prefixed < loops * 9 / 10) continue;

        if (loops > best_loops) {
            best_offset   = offset;
            best_loops    = loops;
            best_prefixed = prefixed;
        }
    }

    if (best_offset < 0) {
        core::LogWarn("UClass::ClassDefaultObject not identified; class defaults unavailable");
        return layout;
    }

    layout.class_default_object = best_offset;
    layout.evidence.push_back(std::format(
        "UClass::ClassDefaultObject at +{:#x}: {}/{} sampled classes point at an array "
        "object that names this class as its own, {} of them named Default__*",
        best_offset, best_loops, classes.size(), best_prefixed));

    core::LogInfo("UClass layout: CDO +{:#x} ({}/{} named Default__*)",
                  best_offset, best_prefixed, best_loops);

    // The naming check is the independent one, so it carries the confidence. The loop is
    // only internal consistency; the name is a fact with a known-correct value.
    layout.confidence = 0.60f + 0.39f * (static_cast<float>(best_prefixed) /
                                         static_cast<float>(best_loops));

    // After the CDO, so its slot can be excluded from the search.
    DeriveInterfaces(memory, array, pool, object_layout, struct_layout, classes, layout);
    return layout;
}

std::vector<core::Address> GetClassInterfaces(core::IMemorySource& memory,
                                              const ObjectArrayInfo& array,
                                              const UClassLayout& layout,
                                              core::Address klass) {
    std::vector<core::Address> found;
    if (!layout.HasInterfaces() || IsNull(klass)) return found;

    const auto data  = core::ReadOr<Address>(memory, klass + layout.interfaces);
    const auto count = core::ReadOr<std::int32_t>(memory, klass + layout.interfaces + 8);
    if (IsNull(data) || count <= 0 || count > 256) return found;

    for (std::int32_t i = 0; i < count; ++i) {
        const auto entry = core::ReadOr<Address>(
            memory, data + static_cast<std::uint64_t>(i) * layout.interface_stride);

        // Re-checked at read time too. Live targets resize this.
        if (IsArrayObject(memory, array, entry)) found.push_back(entry);
    }
    return found;
}

core::Address GetClassDefaultObject(core::IMemorySource& memory, const UClassLayout& layout,
                                    core::Address klass) {
    if (!layout.Valid() || IsNull(klass)) return {};
    return core::ReadOr<Address>(memory, klass + layout.class_default_object);
}

} // namespace zircon::engine
