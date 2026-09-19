#include "il2cpp/Runtime.h"

#include "core/Log.h"
#include "core/ProcessList.h"

#include <algorithm>
#include <format>
#include <array>
#include <filesystem>
#include <cstring>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

namespace zircon::il2cpp {
namespace {

using core::Address;
using core::IsNull;
using core::Raw;

// Is GameAssembly.dll actually mapped into that process yet? A game that has just been
// started has the file on disk long before the runtime is up, and the difference is exactly
// what you wait on before injecting.
bool RuntimeModuleLoaded(std::uint32_t pid) {
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    // Module file names are ASCII in every build anyone ships; anything else won't match
    // the names we are looking for anyway.
    const auto narrow = [](const wchar_t* wide) {
        std::string out;
        for (; *wide; ++wide) out.push_back(static_cast<char>(*wide & 0x7F));
        return out;
    };

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool loaded = false;
    if (::Module32FirstW(snapshot, &entry)) {
        do {
            if (LooksLikeIl2CppModuleName(narrow(entry.szModule))) {
                loaded = true;
                break;
            }
        } while (::Module32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    return loaded;
}

constexpr std::uint16_t kDosMagic = 0x5A4D;   // MZ
constexpr std::uint32_t kPeMagic  = 0x00004550;

// Anything further out than this and the module is not a PE we can read.
constexpr std::uint32_t kMaxExportNames = 65536;
constexpr std::size_t   kMaxNameLength  = 512;

// A 64 MiB GameAssembly has on the order of a million functions. Ten times that is
// past anything real and into a malformed header.
constexpr std::uint32_t kMaxFunctionStarts = 4u * 1000 * 1000;

struct Binding {
    const char*    name;
    Address Api::* slot;
    bool           required;
};

// One row per entry point. A table so the resolver can say exactly which names a stripped
// build is missing, and so adding one is one line.
//
// `required` splits "can't walk this build" from "can walk it, slightly worse". Everything
// past the divider was present on all ten builds in the corpus and is still optional -- a
// name only ever used behind a null check shouldn't be able to refuse a target.
constexpr std::array<Binding, 75> kBindings{{
    {"il2cpp_domain_get",                    &Api::domain_get,                    true},
    {"il2cpp_domain_get_assemblies",         &Api::domain_get_assemblies,         true},
    {"il2cpp_assembly_get_image",            &Api::assembly_get_image,            true},
    {"il2cpp_image_get_name",                &Api::image_get_name,                true},
    {"il2cpp_image_get_class_count",         &Api::image_get_class_count,         true},
    {"il2cpp_image_get_class",               &Api::image_get_class,               true},

    {"il2cpp_class_get_name",                &Api::class_get_name,                true},
    {"il2cpp_class_get_namespace",           &Api::class_get_namespace,           true},
    {"il2cpp_class_get_parent",              &Api::class_get_parent,              true},
    {"il2cpp_class_get_fields",              &Api::class_get_fields,              true},
    {"il2cpp_class_get_methods",             &Api::class_get_methods,             true},
    {"il2cpp_class_get_properties",          &Api::class_get_properties,          true},
    {"il2cpp_class_get_events",              &Api::class_get_events,              true},
    {"il2cpp_class_get_nested_types",        &Api::class_get_nested_types,        true},
    {"il2cpp_class_get_interfaces",          &Api::class_get_interfaces,          true},
    {"il2cpp_class_is_valuetype",            &Api::class_is_valuetype,            true},
    {"il2cpp_class_is_enum",                 &Api::class_is_enum,                 true},
    {"il2cpp_class_get_flags",               &Api::class_get_flags,               true},
    {"il2cpp_class_get_type",                &Api::class_get_type,                true},
    {"il2cpp_class_get_declaring_type",      &Api::class_get_declaring_type,      true},
    {"il2cpp_class_instance_size",           &Api::class_instance_size,           true},

    {"il2cpp_field_get_name",                &Api::field_get_name,                true},
    {"il2cpp_field_get_type",                &Api::field_get_type,                true},
    {"il2cpp_field_get_offset",              &Api::field_get_offset,              true},
    {"il2cpp_field_get_flags",               &Api::field_get_flags,               true},

    {"il2cpp_method_get_name",               &Api::method_get_name,               true},
    {"il2cpp_method_get_return_type",        &Api::method_get_return_type,        true},
    {"il2cpp_method_get_param_count",        &Api::method_get_param_count,        true},
    {"il2cpp_method_get_param",              &Api::method_get_param,              true},
    {"il2cpp_method_get_param_name",         &Api::method_get_param_name,         true},
    {"il2cpp_method_get_flags",              &Api::method_get_flags,              true},

    {"il2cpp_property_get_name",             &Api::property_get_name,             true},
    {"il2cpp_property_get_get_method",       &Api::property_get_get_method,       true},
    {"il2cpp_property_get_set_method",       &Api::property_get_set_method,       true},

    {"il2cpp_type_get_name",                 &Api::type_get_name,                 true},
    {"il2cpp_type_get_type",                 &Api::type_get_type,                 true},
    {"il2cpp_type_get_class_or_element_class", &Api::type_get_class_or_element_class, true},

    {"il2cpp_thread_attach",                 &Api::thread_attach,                 true},
    {"il2cpp_thread_current",                &Api::thread_current,                true},

    // ---- optional from here -----------------------------------------------------
    {"il2cpp_object_header_size",            &Api::object_header_size,            false},
    {"il2cpp_class_value_size",              &Api::class_value_size,              false},
    {"il2cpp_class_enum_basetype",           &Api::class_enum_basetype,           false},
    {"il2cpp_class_is_interface",            &Api::class_is_interface,            false},
    {"il2cpp_class_is_abstract",             &Api::class_is_abstract,             false},
    {"il2cpp_class_is_generic",              &Api::class_is_generic,              false},
    {"il2cpp_class_is_inflated",             &Api::class_is_inflated,             false},
    {"il2cpp_class_is_blittable",            &Api::class_is_blittable,            false},
    {"il2cpp_class_get_image",               &Api::class_get_image,               false},
    {"il2cpp_class_get_type_token",          &Api::class_get_type_token,          false},
    {"il2cpp_class_get_rank",                &Api::class_get_rank,                false},
    {"il2cpp_class_get_element_class",       &Api::class_get_element_class,       false},
    {"il2cpp_class_get_static_field_data",   &Api::class_get_static_field_data,   false},
    {"il2cpp_class_get_data_size",           &Api::class_get_data_size,           false},
    {"il2cpp_class_num_fields",              &Api::class_num_fields,              false},
    {"il2cpp_class_for_each",                &Api::class_for_each,                false},
    {"il2cpp_class_from_type",               &Api::class_from_type,               false},

    {"il2cpp_field_is_literal",              &Api::field_is_literal,              false},
    {"il2cpp_field_get_parent",              &Api::field_get_parent,              false},
    {"il2cpp_field_static_get_value",        &Api::field_static_get_value,        false},

    {"il2cpp_method_get_token",              &Api::method_get_token,              false},
    {"il2cpp_method_get_class",              &Api::method_get_class,              false},
    {"il2cpp_method_is_generic",             &Api::method_is_generic,             false},
    {"il2cpp_method_is_inflated",            &Api::method_is_inflated,            false},
    {"il2cpp_method_is_instance",            &Api::method_is_instance,            false},

    {"il2cpp_property_get_flags",            &Api::property_get_flags,            false},
    {"il2cpp_property_get_parent",           &Api::property_get_parent,           false},

    {"il2cpp_type_is_byref",                 &Api::type_is_byref,                 false},
    {"il2cpp_type_is_pointer_type",          &Api::type_is_pointer_type,          false},
    {"il2cpp_type_is_static",                &Api::type_is_static,                false},
    {"il2cpp_type_get_attrs",                &Api::type_get_attrs,                false},
    {"il2cpp_type_get_assembly_qualified_name", &Api::type_get_assembly_qualified_name, false},

    {"il2cpp_image_get_filename",            &Api::image_get_filename,            false},
    {"il2cpp_image_get_assembly",            &Api::image_get_assembly,            false},

    {"il2cpp_thread_detach",                 &Api::thread_detach,                 false},
    {"il2cpp_free",                          &Api::free,                          false},
}};

std::string ReadName(core::IMemorySource& memory, Address at) {
    return core::ReadCString(memory, at, kMaxNameLength);
}

std::string Join(const std::vector<std::string>& names) {
    std::string joined;
    for (const auto& name : names) {
        if (!joined.empty()) joined += ", ";
        joined += name;
    }
    return joined;
}

} // namespace

int RequiredEntryPointCount() {
    int n = 0;
    for (const auto& binding : kBindings) n += binding.required ? 1 : 0;
    return n;
}

int OptionalEntryPointCount() {
    return static_cast<int>(kBindings.size()) - RequiredEntryPointCount();
}

bool LooksLikeIl2CppModuleName(std::string_view name) {
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower == "gameassembly.dll" || lower == "libil2cpp.so" ||
           lower == "unityframework"   || lower == "libil2cpp.dylib";
}

std::vector<core::PeExport> ReadExports(core::IMemorySource& memory, Address module_base) {
    std::vector<core::PeExport> out;
    if (IsNull(module_base)) return out;

    const auto dos = core::ReadOr<std::uint16_t>(memory, module_base);
    if (dos != kDosMagic) return out;

    const auto e_lfanew = core::ReadOr<std::int32_t>(memory, module_base + 0x3C);
    if (e_lfanew <= 0 || e_lfanew > 0x10000) return out;

    const Address nt = module_base + static_cast<std::uint64_t>(e_lfanew);
    if (core::ReadOr<std::uint32_t>(memory, nt) != kPeMagic) return out;

    // The data directory sits at a different offset in PE32 and PE32+, and the magic in the
    // optional header is the only thing that says which this is.
    const auto magic = core::ReadOr<std::uint16_t>(memory, nt + 0x18);
    const bool pe64  = magic == 0x20B;
    const Address data_dir = nt + 0x18 + (pe64 ? 0x70 : 0x60);

    const auto export_rva = core::ReadOr<std::uint32_t>(memory, data_dir);
    if (export_rva == 0) return out;

    const Address dir = module_base + export_rva;
    const auto name_count = core::ReadOr<std::uint32_t>(memory, dir + 0x18);
    const auto names_rva  = core::ReadOr<std::uint32_t>(memory, dir + 0x20);
    const auto ords_rva   = core::ReadOr<std::uint32_t>(memory, dir + 0x24);
    const auto funcs_rva  = core::ReadOr<std::uint32_t>(memory, dir + 0x1C);
    const auto ord_base   = core::ReadOr<std::uint32_t>(memory, dir + 0x10);

    if (name_count == 0 || name_count > kMaxExportNames) return out;
    if (names_rva == 0 || ords_rva == 0 || funcs_rva == 0) return out;

    out.reserve(name_count);
    for (std::uint32_t i = 0; i < name_count; ++i) {
        const auto name_rva = core::ReadOr<std::uint32_t>(memory, module_base + names_rva + i * 4);
        if (name_rva == 0) continue;

        std::string name = ReadName(memory, module_base + name_rva);
        if (name.empty()) continue;

        const auto ordinal = core::ReadOr<std::uint16_t>(memory, module_base + ords_rva + i * 2);
        const auto func_rva = core::ReadOr<std::uint32_t>(memory,
                                                          module_base + funcs_rva + ordinal * 4);
        if (func_rva == 0) continue;

        out.push_back(core::PeExport{std::move(name), func_rva,
                                     static_cast<std::uint16_t>(ordinal + ord_base)});
    }
    return out;
}

std::vector<std::uint32_t> ReadFunctionStarts(core::IMemorySource& memory,
                                              Address module_base) {
    std::vector<std::uint32_t> starts;
    if (IsNull(module_base)) return starts;

    if (core::ReadOr<std::uint16_t>(memory, module_base) != kDosMagic) return starts;
    const auto e_lfanew = core::ReadOr<std::int32_t>(memory, module_base + 0x3C);
    if (e_lfanew <= 0 || e_lfanew > 0x10000) return starts;

    const Address nt = module_base + static_cast<std::uint64_t>(e_lfanew);
    if (core::ReadOr<std::uint32_t>(memory, nt) != kPeMagic) return starts;
    if (core::ReadOr<std::uint16_t>(memory, nt + 0x18) != 0x20B) return starts;   // x64 only

    // Directory 3 is IMAGE_DIRECTORY_ENTRY_EXCEPTION, and on x64 it is an array of
    // RUNTIME_FUNCTION: begin RVA, end RVA, unwind info RVA, twelve bytes each.
    const Address entry = nt + 0x18 + 0x70 + 3 * 8;
    const auto dir_rva  = core::ReadOr<std::uint32_t>(memory, entry);
    const auto dir_size = core::ReadOr<std::uint32_t>(memory, entry + 4);
    if (dir_rva == 0 || dir_size < 12) return starts;

    const std::uint32_t count = std::min<std::uint32_t>(dir_size / 12, kMaxFunctionStarts);
    starts.reserve(count);

    std::vector<std::uint32_t> block(count * 3);
    if (!core::ReadArrayInto(memory, module_base + dir_rva, block.data(), block.size()))
        return starts;

    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t begin = block[i * 3];
        const std::uint32_t end   = block[i * 3 + 1];
        if (begin == 0 || end <= begin) continue;
        starts.push_back(begin);
    }

    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    return starts;
}

std::vector<std::pair<std::string, std::uint32_t>> ResolvedEntryPoints(const RuntimeInfo& runtime) {
    std::vector<std::pair<std::string, std::uint32_t>> out;
    if (IsNull(runtime.module_base)) return out;

    out.reserve(kBindings.size());
    for (const auto& [name, slot, required] : kBindings) {
        const Address at = runtime.api.*slot;
        if (IsNull(at)) continue;
        out.emplace_back(name, static_cast<std::uint32_t>(Raw(at) - Raw(runtime.module_base)));
    }
    return out;
}

std::optional<RuntimeInfo> FindRuntime(core::IMemorySource& memory) {
    // Named modules first, then everything else. The name is a shortcut, not the test --
    // a renamed GameAssembly still exports the API.
    std::vector<core::ModuleInfo> ordered;
    for (const auto& module : memory.Modules())
        if (LooksLikeIl2CppModuleName(module.name)) ordered.push_back(module);
    for (const auto& module : memory.Modules())
        if (!LooksLikeIl2CppModuleName(module.name)) ordered.push_back(module);

    RuntimeInfo best;

    for (const auto& module : ordered) {
        const auto exports = ReadExports(memory, module.base);
        if (exports.empty()) continue;

        int il2cpp_count = 0;
        std::unordered_map<std::string, std::uint32_t> by_name;
        by_name.reserve(exports.size());
        for (const auto& e : exports) {
            by_name.emplace(e.name, e.rva);
            if (e.name.rfind("il2cpp_", 0) == 0) ++il2cpp_count;
        }
        if (il2cpp_count == 0) continue;

        RuntimeInfo candidate;
        candidate.module_base    = module.base;
        candidate.module_size    = module.size;
        candidate.module_name    = module.name;
        candidate.il2cpp_exports = il2cpp_count;

        for (const auto& [name, slot, required] : kBindings) {
            const auto it = by_name.find(name);
            if (it == by_name.end()) {
                (required ? candidate.api.missing : candidate.api.degraded).emplace_back(name);
                continue;
            }
            candidate.api.*slot = module.base + it->second;
            ++(required ? candidate.api.resolved : candidate.api.enrichment);
        }

        candidate.evidence.push_back(std::format(
            "{} exports {} il2cpp_* entry points; {} of {} the walk needs resolved",
            module.name, il2cpp_count, candidate.api.resolved, RequiredEntryPointCount()));

        if (candidate.api.Complete()) {
            candidate.confidence = 0.98f;
            candidate.evidence.emplace_back(
                "every entry point resolved by name, so no metadata version is involved");
        } else {
            // Partial is still the best answer we have. Naming the gaps is the point.
            candidate.confidence = 0.5f * static_cast<float>(candidate.api.resolved) /
                                   static_cast<float>(RequiredEntryPointCount());
            candidate.evidence.push_back(std::format("missing: {}", Join(candidate.api.missing)));
        }

        if (!candidate.api.degraded.empty()) {
            candidate.evidence.push_back(std::format(
                "{} of {} optional entry points absent, so the walk runs without them: {}",
                candidate.api.degraded.size(), OptionalEntryPointCount(),
                Join(candidate.api.degraded)));
        }

        if (candidate.api.resolved > best.api.resolved) best = std::move(candidate);
        if (best.api.Complete() && best.api.degraded.empty()) break;   // nothing beats a full set
    }

    if (best.api.resolved == 0) return std::nullopt;

    core::LogInfo("il2cpp runtime: {} at {:#x}, {} exports, {}/{} entry points, {}/{} optional",
                  best.module_name, Raw(best.module_base), best.il2cpp_exports,
                  best.api.resolved, RequiredEntryPointCount(),
                  best.api.enrichment, OptionalEntryPointCount());
    return best;
}

std::vector<UnityProcess> DetectUnityProcesses() {
    std::vector<UnityProcess> found;
    std::error_code ec;

    // Unity ships its crash handler beside the game, so it passes the folder test. Named,
    // because nothing about the process itself tells it apart.
    const auto is_helper = [](std::string_view name) {
        return name == "UnityCrashHandler64.exe" || name == "UnityCrashHandler32.exe";
    };

    for (auto& process : core::EnumerateProcesses()) {
        if (process.path.empty()) continue;      // no access to look, so no claim either way
        if (is_helper(process.name)) continue;

        const auto folder = std::filesystem::path(process.path).parent_path();
        if (!std::filesystem::exists(folder / "GameAssembly.dll", ec)) continue;

        UnityProcess entry;
        entry.runtime_loaded = RuntimeModuleLoaded(process.pid);
        entry.process        = std::move(process);
        found.push_back(std::move(entry));
    }
    return found;
}

} // namespace zircon::il2cpp
