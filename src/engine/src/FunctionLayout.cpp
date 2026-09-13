#include "engine/FunctionLayout.h"
#include "core/Log.h"
#include "core/PeImage.h"

#include <algorithm>
#include <format>
#include <set>

namespace zircon::engine {
namespace {

using core::Address;
using core::IsNull;
using core::Raw;

constexpr int kMaxProbe       = 0xE0;
constexpr int kSampleTarget   = 400;
constexpr int kMaxChainLength = 8192;

bool IsArrayObject(core::IMemorySource& memory, const ObjectArrayInfo& array, Address candidate) {
    if (IsNull(candidate)) return false;
    std::int32_t index{};
    if (!core::ReadInto(memory, candidate + array.index_offset, index)) return false;
    if (index < 0 || index >= array.num_elements) return false;
    return Raw(ObjectAt(memory, array, index)) == Raw(candidate);
}

// Ranges come from the loaded modules' PE section headers, never from page protection. A
// minidump captured without MiniDumpWithFullMemoryInfo records no protection at all, so the
// Dump provider calls everything executable and this test passes for any pointer. That is
// precisely how a UStruct member full of FProperty pointers once got picked as
// UFunction::Func.
class CodeRanges {
public:
    explicit CodeRanges(core::IMemorySource& memory)
        : ranges_(core::ExecutableRanges(memory)) {}

    bool Contains(Address addr) const {
        if (IsNull(addr)) return false;
        const auto it = std::upper_bound(ranges_.begin(), ranges_.end(), Raw(addr),
            [](std::uint64_t value, const core::RegionInfo& r) { return value < Raw(r.base); });
        if (it == ranges_.begin()) return false;

        const auto& candidate = *(it - 1);
        return Raw(addr) >= Raw(candidate.base) &&
               Raw(addr) < Raw(candidate.base) + candidate.size;
    }

    bool Empty() const { return ranges_.empty(); }

private:
    std::vector<core::RegionInfo> ranges_;
};

} // namespace

UFunctionLayout DeriveFunctionLayout(core::IMemorySource& memory,
                                     const ObjectArrayInfo& array,
                                     const NamePoolInfo& pool,
                                     const UObjectLayout& object_layout,
                                     const UStructLayout& struct_layout,
                                     const FPropertyLayout& property_layout) {
    UFunctionLayout layout;
    if (!struct_layout.Valid() || struct_layout.children < 0) return layout;

    std::vector<Address> functions;
    std::vector<Address> classes_with_children;

    for (std::int32_t index = 0;
         index < array.num_elements && static_cast<int>(functions.size()) < kSampleTarget;
         ++index) {
        const auto object = ObjectAt(memory, array, index);
        if (IsNull(object)) continue;

        const std::string kind = GetClassName(memory, object_layout, pool, object);
        if (kind == "Function") {
            functions.push_back(object);
        } else if (kind == "Class" && classes_with_children.size() < 200) {
            const auto head = core::ReadOr<Address>(memory, object + struct_layout.children);
            if (IsArrayObject(memory, array, head)) classes_with_children.push_back(object);
        }
    }

    if (functions.size() < 32) {
        core::LogWarn("only {} UFunction objects sampled; cannot derive the function layout",
                      functions.size());
        return layout;
    }

    // --- UField::Next -----------------------------------------------------------------
    // Walked from UStruct::Children. Every link lands on another array object and the chain
    // terminates; one that loops or leaves the array isn't the field list.
    for (int offset = object_layout.outer_offset + 8;
         offset <= kMaxProbe && layout.field_next < 0; offset += 8) {
        if (offset == struct_layout.super_struct || offset == struct_layout.children ||
            offset == struct_layout.child_properties)
            continue;

        int good = 0;
        for (const auto klass : classes_with_children) {
            Address current = core::ReadOr<Address>(memory, klass + struct_layout.children);
            std::set<std::uint64_t> seen;
            bool sane = true;

            for (int step = 0; step < kMaxChainLength; ++step) {
                const auto next = core::ReadOr<Address>(memory, current + offset);
                if (IsNull(next)) break;
                if (!IsArrayObject(memory, array, next))    { sane = false; break; }
                if (!seen.insert(Raw(next)).second)         { sane = false; break; }
                current = next;
            }
            if (sane) ++good;
        }

        if (!classes_with_children.empty() &&
            good >= static_cast<int>(classes_with_children.size()) * 9 / 10) {
            layout.field_next = offset;
            layout.evidence.push_back(std::format(
                "UField::Next at +{:#x}: {}/{} Children chains stay in the object array "
                "and terminate", offset, good, classes_with_children.size()));
        }
    }

    // --- UFunction::FunctionFlags -----------------------------------------------------
    // Found through a reflection-system invariant, not a bit pattern. Every UFunction
    // has exactly one access specifier, so exactly one of Public, Private and Protected is
    // set. Random dwords don't manage that at scale.
    {
        int best_offset = -1, best_hits = 0;
        const std::uint32_t access = function_flags::kPublic | function_flags::kPrivate |
                                     function_flags::kProtected;

        for (int offset = struct_layout.properties_size; offset <= kMaxProbe; offset += 4) {
            int hits = 0;
            for (const auto function : functions) {
                const auto value = core::ReadOr<std::uint32_t>(memory, function + offset);
                const std::uint32_t bits = value & access;
                if (bits != 0 && (bits & (bits - 1)) == 0) ++hits;
            }
            if (hits > best_hits) {
                best_hits   = hits;
                best_offset = offset;
            }
        }

        if (best_offset >= 0 && best_hits >= static_cast<int>(functions.size()) * 9 / 10) {
            layout.function_flags = best_offset;
            layout.evidence.push_back(std::format(
                "UFunction::FunctionFlags at +{:#x}: {}/{} have exactly one access "
                "specifier bit", best_offset, best_hits, functions.size()));
        }
    }

    const CodeRanges code(memory);
    if (code.Empty())
        core::LogWarn("no executable sections found; UFunction::Func cannot be "
                      "identified reliably");

    // --- UFunction::Func --------------------------------------------------------------
    // "Points into executable memory" is necessary but NOT sufficient, and ranking on it
    // alone picks the wrong field. A UFunction holds more than one code pointer, and a slot
    // holding the same shared thunk for every function satisfies the test on 100% of
    // samples, beating the real Func field wherever a few entries are null.
    //
    // Not hypothetical. It produced a dump in which all 14632 functions shared one address,
    // and the two dumps only disagreed because the live run happened to score the right
    // field marginally higher.
    //
    // What actually characterises Func is pointing somewhere *different* per function. Rank
    // by distinct targets instead of hit count and it separates from every thunk slot.
    {
        int best_offset = -1;
        std::size_t best_distinct = 0;
        int best_hits = 0;

        for (int offset = struct_layout.properties_size; offset <= kMaxProbe; offset += 8) {
            int hits = 0;
            std::set<std::uint64_t> targets;

            for (const auto function : functions) {
                const auto target = core::ReadOr<Address>(memory, function + offset);
                if (!code.Contains(target)) continue;
                ++hits;
                targets.insert(Raw(target));
            }

            if (hits < static_cast<int>(functions.size()) * 9 / 10) continue;
            if (targets.size() > best_distinct) {
                best_distinct = targets.size();
                best_offset   = offset;
                best_hits     = hits;
            }
        }

        // Script-only functions legitimately share the interpreter thunk, so expect some
        // repetition. A handful of distinct targets across hundreds of functions is not
        // repetition, it's the wrong field.
        if (best_offset >= 0 && best_distinct >= functions.size() / 8) {
            layout.native_func = best_offset;
            layout.evidence.push_back(std::format(
                "UFunction::Func at +{:#x}: {}/{} point into executable memory, at {} "
                "distinct addresses", best_offset, best_hits, functions.size(),
                best_distinct));
        } else if (best_offset >= 0) {
            core::LogWarn("candidate Func offset +{:#x} has only {} distinct targets across "
                          "{} functions; rejecting it as a shared thunk slot",
                          best_offset, best_distinct, functions.size());
        }
    }

    int resolved = 0;
    for (const int slot : {layout.field_next, layout.function_flags, layout.native_func})
        if (slot >= 0) ++resolved;
    layout.confidence = layout.Valid() ? std::min(0.96f, 0.55f + 0.14f * resolved) : 0.0f;

    (void)property_layout;
    core::LogInfo("UFunction layout: next +{:#x}, flags +{:#x}, func +{:#x}",
                  layout.field_next, layout.function_flags, layout.native_func);
    return layout;
}

std::vector<std::string> DescribeFunctionFlags(std::uint32_t flags) {
    using namespace function_flags;
    static const std::pair<std::uint32_t, const char*> kNamed[] = {
        {kFinal, "Final"},     {kNative, "Native"},   {kEvent, "Event"},
        {kStatic, "Static"},   {kPublic, "Public"},   {kPrivate, "Private"},
        {kProtected, "Protected"},
    };

    std::vector<std::string> names;
    for (const auto& [bit, name] : kNamed)
        if (flags & bit) names.emplace_back(name);
    return names;
}

std::vector<std::string> DescribePropertyFlags(std::uint64_t flags) {
    static const std::pair<std::uint64_t, const char*> kNamed[] = {
        {0x0000000000000001ull, "Edit"},
        {0x0000000000000002ull, "ConstParm"},
        {0x0000000000000004ull, "BlueprintVisible"},
        {0x0000000000000008ull, "ExportObject"},
        {0x0000000000000010ull, "BlueprintReadOnly"},
        {0x0000000000000020ull, "Net"},
        {0x0000000000000040ull, "EditFixedSize"},
        {0x0000000000000080ull, "Parm"},
        {0x0000000000000100ull, "OutParm"},
        {0x0000000000000200ull, "ZeroConstructor"},
        {0x0000000000000400ull, "ReturnParm"},
        {0x0000000000000800ull, "DisableEditOnTemplate"},
        {0x0000000000001000ull, "NonNullable"},
        {0x0000000000002000ull, "Transient"},
        {0x0000000000004000ull, "Config"},
        {0x0000000000008000ull, "RequiredParm"},
        {0x0000000000010000ull, "DisableEditOnInstance"},
        {0x0000000000020000ull, "EditConst"},
        {0x0000000000040000ull, "GlobalConfig"},
        {0x0000000000080000ull, "InstancedReference"},
        {0x0000000000200000ull, "DuplicateTransient"},
        {0x0000000001000000ull, "SaveGame"},
        {0x0000000002000000ull, "NoClear"},
        {0x0000000008000000ull, "ReferenceParm"},
        {0x0000000010000000ull, "BlueprintAssignable"},
        {0x0000000020000000ull, "Deprecated"},
        {0x0000000040000000ull, "IsPlainOldData"},
        {0x0000000080000000ull, "RepSkip"},
        {0x0000000100000000ull, "RepNotify"},
        {0x0000000200000000ull, "Interp"},
        {0x0000000400000000ull, "NonTransactional"},
        {0x0000000800000000ull, "EditorOnly"},
        {0x0000001000000000ull, "NoDestructor"},
        {0x0000004000000000ull, "AutoWeak"},
        {0x0000008000000000ull, "ContainsInstancedReference"},
        {0x0000010000000000ull, "AssetRegistrySearchable"},
        {0x0000020000000000ull, "SimpleDisplay"},
        {0x0000040000000000ull, "AdvancedDisplay"},
        {0x0000080000000000ull, "Protected"},
        {0x0000100000000000ull, "BlueprintCallable"},
        {0x0000200000000000ull, "BlueprintAuthorityOnly"},
        {0x0000400000000000ull, "TextExportTransient"},
        {0x0000800000000000ull, "NonPIEDuplicateTransient"},
        {0x0001000000000000ull, "ExposeOnSpawn"},
        {0x0002000000000000ull, "PersistentInstance"},
        {0x0004000000000000ull, "UObjectWrapper"},
        {0x0008000000000000ull, "HasGetValueTypeHash"},
        {0x0010000000000000ull, "NativeAccessSpecifierPublic"},
        {0x0020000000000000ull, "NativeAccessSpecifierProtected"},
        {0x0040000000000000ull, "NativeAccessSpecifierPrivate"},
        {0x0080000000000000ull, "SkipSerialization"},
        {0x0100000000000000ull, "TObjectPtr"},          // UE5
    };

    std::vector<std::string> names;
    std::uint64_t recognised = 0;

    for (const auto& [bit, name] : kNamed) {
        if (!(flags & bit)) continue;
        names.emplace_back(name);
        recognised |= bit;
    }

    // A bit we have no name for is still information. Reporting it keeps the named list
    // faithful to the raw word instead of quietly narrower.
    if (const std::uint64_t rest = flags & ~recognised; rest != 0)
        names.push_back(std::format("Unknown({:#x})", rest));

    return names;
}

std::vector<Address> GetClassFunctions(core::IMemorySource& memory,
                                       const ObjectArrayInfo& array,
                                       const NamePoolInfo& pool,
                                       const UObjectLayout& object_layout,
                                       const UStructLayout& struct_layout,
                                       const UFunctionLayout& function_layout,
                                       Address klass) {
    std::vector<Address> found;
    if (IsNull(klass) || struct_layout.children < 0 || function_layout.field_next < 0)
        return found;

    std::set<std::uint64_t> seen;
    Address current = core::ReadOr<Address>(memory, klass + struct_layout.children);

    while (!IsNull(current) && seen.insert(Raw(current)).second &&
           found.size() < kMaxChainLength) {
        // Children carries every UField, not only functions: enums and nested structs
        // ride the same list, so filter; don't assume.
        if (ClassifyObject(memory, object_layout, struct_layout, pool, current) ==
            ObjectKind::Function)
            found.push_back(current);

        if (!IsArrayObject(memory, array, current)) break;
        current = core::ReadOr<Address>(memory, current + function_layout.field_next);
    }
    return found;
}

std::uint32_t GetFunctionFlags(core::IMemorySource& memory, const UFunctionLayout& layout,
                               Address function) {
    if (IsNull(function) || layout.function_flags < 0) return 0;
    return core::ReadOr<std::uint32_t>(memory, function + layout.function_flags);
}

Address GetNativeFunc(core::IMemorySource& memory, const UFunctionLayout& layout,
                      Address function) {
    if (IsNull(function) || layout.native_func < 0) return {};
    return core::ReadOr<Address>(memory, function + layout.native_func);
}

} // namespace zircon::engine
