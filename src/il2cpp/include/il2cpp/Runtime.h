#pragma once

// Finding the IL2CPP runtime in a target and resolving the entry points the walk needs.
//
// Unity compiles C# -> C++ -> native. The type info ends up in global-metadata.dat plus
// registration structs in the binary, and both change shape per metadata version. That's why
// every dumper that parses them carries a per-version struct table and breaks on each Unity
// release.
//
// We don't parse any of it. GameAssembly.dll exports the IL2CPP embedding C API by name, a
// public contract that's been stable since 5.x, so we ask the runtime instead. No version
// knowledge, and encrypted metadata doesn't matter -- the runtime already decrypted it.
//
// Real failure mode: a build that strips or renames its exports. Reported, not guessed
// around.

#include "core/MemorySource.h"
#include "core/PeImage.h"
#include "core/ProcessList.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace zircon::il2cpp {

// Every entry point the walk can use, as addresses in the target.
//
// Named, not indexed, so a missing one can be reported by name. "doesn't export
// il2cpp_class_get_nested_types" is actionable; "export 14 missing" isn't.
//
// Two groups. Required is what the walk can't run without. Optional only improves the answer
// and is null-checked wherever it's used. Demanding all of them would refuse a stripped build
// the walk could have handled fine.
struct Api {
    // ---- required ---------------------------------------------------------------
    core::Address domain_get{};
    core::Address domain_get_assemblies{};
    core::Address assembly_get_image{};
    core::Address image_get_name{};
    core::Address image_get_class_count{};
    core::Address image_get_class{};

    core::Address class_get_name{};
    core::Address class_get_namespace{};
    core::Address class_get_parent{};
    core::Address class_get_fields{};
    core::Address class_get_methods{};
    core::Address class_get_properties{};
    core::Address class_get_events{};
    core::Address class_get_nested_types{};
    core::Address class_get_interfaces{};
    core::Address class_is_valuetype{};
    core::Address class_is_enum{};
    core::Address class_get_flags{};
    core::Address class_get_type{};
    core::Address class_get_declaring_type{};
    core::Address class_instance_size{};

    core::Address field_get_name{};
    core::Address field_get_type{};
    core::Address field_get_offset{};
    core::Address field_get_flags{};

    core::Address method_get_name{};
    core::Address method_get_return_type{};
    core::Address method_get_param_count{};
    core::Address method_get_param{};
    core::Address method_get_param_name{};
    core::Address method_get_flags{};

    core::Address property_get_name{};
    core::Address property_get_get_method{};
    core::Address property_get_set_method{};

    core::Address type_get_name{};
    core::Address type_get_type{};
    core::Address type_get_class_or_element_class{};

    core::Address thread_attach{};
    core::Address thread_current{};

    // ---- optional ---------------------------------------------------------------
    // Present on all ten builds in the corpus, Unity 2019 to 6.x. Still optional on purpose.
    core::Address object_header_size{};          // the value-type header trap, answered
    core::Address class_value_size{};
    core::Address class_enum_basetype{};
    core::Address class_is_interface{};
    core::Address class_is_abstract{};
    core::Address class_is_generic{};
    core::Address class_is_inflated{};
    core::Address class_is_blittable{};
    core::Address class_get_image{};
    core::Address class_get_type_token{};
    core::Address class_get_rank{};
    core::Address class_get_element_class{};
    core::Address class_get_static_field_data{};
    core::Address class_get_data_size{};
    core::Address class_num_fields{};            // bounds the field walk, see below
    core::Address class_for_each{};              // every class, inflated generics included
    core::Address class_from_type{};

    core::Address field_is_literal{};
    core::Address field_get_parent{};
    core::Address field_static_get_value{};

    core::Address method_get_token{};
    core::Address method_get_class{};
    core::Address method_is_generic{};
    core::Address method_is_inflated{};
    core::Address method_is_instance{};

    core::Address property_get_flags{};
    core::Address property_get_parent{};

    core::Address type_is_byref{};
    core::Address type_is_pointer_type{};
    core::Address type_is_static{};
    core::Address type_get_attrs{};
    core::Address type_get_assembly_qualified_name{};

    core::Address image_get_filename{};
    core::Address image_get_assembly{};

    core::Address thread_detach{};

    // The runtime's deallocator. Two string getters return memory we own; without this a
    // full walk leaks a few hundred thousand strings into the game's heap.
    core::Address free{};

    // Every required entry point resolved. A partial set still gets reported -- which ones
    // are missing says what kind of build this is.
    bool Complete() const { return missing.empty(); }

    std::vector<std::string> missing;   // required, absent
    std::vector<std::string> degraded;  // optional, absent -- the walk runs without them
    int resolved{0};                    // required only, so the count means one thing
    int enrichment{0};                  // optional, resolved
};

// Group sizes, so "37/39" has a denominator that doesn't move silently when a row is added.
int RequiredEntryPointCount();
int OptionalEntryPointCount();

struct RuntimeInfo {
    core::Address module_base{};
    std::uint64_t module_size{};
    std::string   module_name;     // GameAssembly.dll, libil2cpp.so, UnityFramework

    // Total il2cpp_* exports, not just the ones we use. ~240 is stock; a handful means
    // stripped, and that's worth saying.
    int il2cpp_exports{0};

    Api api;

    float confidence{0.0f};
    std::vector<std::string> evidence;

    bool Valid() const { return !core::IsNull(module_base) && api.Complete(); }
};

// Module names worth checking first. Not a filter: anything that exports the API qualifies,
// whatever it's called. That's what catches renamed builds.
bool LooksLikeIl2CppModuleName(std::string_view name);

// Reads a module's export table out of the target, not off disk. Same code for live,
// minidump and static, and it sees what the process actually has, which matters once a
// loader or packer has been at it.
std::vector<core::PeExport> ReadExports(core::IMemorySource& memory, core::Address module_base);

// Function entry points from the module's exception directory, as sorted RVAs.
//
// Used as an independent answer to "is this the start of a function". The runtime's own
// structures can't answer that without being trusted first; .pdata comes from the linker.
// Empty on 32-bit (no exception directory) or a stripped module.
std::vector<std::uint32_t> ReadFunctionStarts(core::IMemorySource& memory,
                                              core::Address module_base);

// Resolved entry points as (name, RVA), table order. An export map of the runtime, for the
// dump header.
std::vector<std::pair<std::string, std::uint32_t>> ResolvedEntryPoints(const RuntimeInfo& runtime);

// Locates the IL2CPP module and resolves the API. Nothing when no module exports the entry
// points -- a Mono or non-Unity game.
std::optional<RuntimeInfo> FindRuntime(core::IMemorySource& memory);

struct UnityProcess {
    core::ProcessInfo process;

    // The module is mapped, not merely sitting next to the executable. A game that has just
    // started passes the first test minutes before the second.
    bool runtime_loaded{false};
};

// Running processes with GameAssembly.dll beside them. Folder-based on purpose: it answers
// before the runtime is up, which is when you want to know. A Unity game with UnityPlayer
// and no GameAssembly is the Mono backend and is left out, because this backend cannot
// touch it.
std::vector<UnityProcess> DetectUnityProcesses();

} // namespace zircon::il2cpp
