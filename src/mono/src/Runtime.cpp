#include "mono/Runtime.h"

#include "core/Log.h"
#include "core/ModuleExports.h"
#include "core/ProcessList.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <format>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

namespace zircon::mono {
namespace {

using core::Address;
using core::IsNull;
using core::Raw;

struct Binding {
    const char*    name;
    Address Api::* slot;
    bool           required;
};

constexpr Binding kBindings[] = {
    {"mono_get_root_domain",            &Api::get_root_domain,            true},
    {"mono_domain_assembly_foreach",    &Api::domain_assembly_foreach,    true},
    {"mono_assembly_get_image",         &Api::assembly_get_image,         true},
    {"mono_image_get_name",             &Api::image_get_name,             true},
    {"mono_image_get_table_rows",       &Api::image_get_table_rows,       true},
    {"mono_class_get",                  &Api::class_get,                  true},

    {"mono_class_get_name",             &Api::class_get_name,             true},
    {"mono_class_get_namespace",        &Api::class_get_namespace,        true},
    {"mono_class_get_parent",           &Api::class_get_parent,           true},
    {"mono_class_get_flags",            &Api::class_get_flags,            true},
    {"mono_class_get_type",             &Api::class_get_type,             true},
    {"mono_class_get_fields",           &Api::class_get_fields,           true},
    {"mono_class_get_methods",          &Api::class_get_methods,          true},
    {"mono_class_get_properties",       &Api::class_get_properties,       true},
    {"mono_class_get_interfaces",       &Api::class_get_interfaces,       true},
    {"mono_class_get_nested_types",     &Api::class_get_nested_types,     true},
    {"mono_class_is_valuetype",         &Api::class_is_valuetype,         true},
    {"mono_class_is_enum",              &Api::class_is_enum,              true},
    {"mono_class_instance_size",        &Api::class_instance_size,        true},
    {"mono_class_init",                 &Api::class_init,                 true},

    {"mono_field_get_name",             &Api::field_get_name,             true},
    {"mono_field_get_type",             &Api::field_get_type,             true},
    {"mono_field_get_offset",           &Api::field_get_offset,           true},
    {"mono_field_get_flags",            &Api::field_get_flags,            true},

    {"mono_method_get_name",            &Api::method_get_name,            true},
    {"mono_method_get_flags",           &Api::method_get_flags,           true},

    {"mono_type_get_type",              &Api::type_get_type,              true},
    {"mono_type_get_class",             &Api::type_get_class,             true},

    {"mono_thread_attach",              &Api::thread_attach,              true},

    {"mono_image_get_table_info",       &Api::image_get_table_info,       false},
    {"mono_method_signature",           &Api::method_signature,           false},
    {"mono_signature_get_return_type",  &Api::signature_get_return_type,  false},
    {"mono_signature_get_params",       &Api::signature_get_params,       false},
    {"mono_signature_get_param_count",  &Api::signature_get_param_count,  false},
    {"mono_type_get_name",              &Api::type_get_name,              false},
    {"mono_class_get_nesting_type",     &Api::class_get_nesting_type,     false},
    {"mono_class_get_image",            &Api::class_get_image,            false},
    {"mono_class_get_type_token",       &Api::class_get_type_token,       false},
    {"mono_class_get_element_class",    &Api::class_get_element_class,    false},
    {"mono_class_value_size",           &Api::class_value_size,           false},
    {"mono_class_min_align",            &Api::class_min_align,            false},
    {"mono_class_num_fields",           &Api::class_num_fields,           false},
    {"mono_class_num_methods",          &Api::class_num_methods,          false},
    {"mono_class_num_properties",       &Api::class_num_properties,       false},
    {"mono_class_get_events",           &Api::class_get_events,           false},
    {"mono_class_num_events",           &Api::class_num_events,           false},
    {"mono_class_is_delegate",          &Api::class_is_delegate,          false},
    {"mono_class_is_blittable",         &Api::class_is_blittable,         false},
    {"mono_class_from_mono_type",       &Api::class_from_mono_type,       false},

    {"mono_field_get_parent",           &Api::field_get_parent,           false},

    {"mono_method_get_token",           &Api::method_get_token,           false},
    {"mono_method_get_class",           &Api::method_get_class,           false},
    {"mono_method_get_header",          &Api::method_get_header,          false},

    {"mono_property_get_name",          &Api::property_get_name,          false},
    {"mono_property_get_get_method",    &Api::property_get_get_method,    false},
    {"mono_property_get_set_method",    &Api::property_get_set_method,    false},
    {"mono_property_get_flags",         &Api::property_get_flags,         false},

    {"mono_event_get_name",             &Api::event_get_name,             false},

    {"mono_type_get_name_full",         &Api::type_get_name_full,         false},
    {"mono_type_is_byref",              &Api::type_is_byref,              false},

    {"mono_assembly_get_name",          &Api::assembly_get_name,          false},
    {"mono_assembly_name_get_name",     &Api::assembly_name_get_name,     false},
    {"mono_assembly_name_get_version",  &Api::assembly_name_get_version,  false},
    {"mono_image_get_filename",         &Api::image_get_filename,         false},
    {"mono_image_get_assembly",         &Api::image_get_assembly,         false},

    {"mono_unity_class_get_generic_argument_count", &Api::unity_generic_argument_count, false},
    {"mono_unity_class_get_generic_argument_at",    &Api::unity_generic_argument_at,    false},

    {"mono_thread_detach",              &Api::thread_detach,              false},
    {"mono_free",                       &Api::free,                       false},
};

std::string Join(const std::vector<std::string>& names) {
    std::string joined;
    for (const auto& name : names) {
        if (!joined.empty()) joined += ", ";
        joined += name;
    }
    return joined;
}

std::string Lower(std::string_view text) {
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

bool RuntimeModuleLoaded(std::uint32_t pid) {
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

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
            if (LooksLikeMonoModuleName(narrow(entry.szModule))) {
                loaded = true;
                break;
            }
        } while (::Module32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    return loaded;
}

} // namespace

int RequiredEntryPointCount() {
    int n = 0;
    for (const auto& b : kBindings) if (b.required) ++n;
    return n;
}

int OptionalEntryPointCount() {
    int n = 0;
    for (const auto& b : kBindings) if (!b.required) ++n;
    return n;
}

bool LooksLikeMonoModuleName(std::string_view name) {
    const auto lower = Lower(name);
    if (lower.rfind("mono-2.0", 0) == 0) return true;
    if (lower.rfind("libmonobdwgc", 0) == 0) return true;
    if (lower.rfind("libmonosgen", 0) == 0) return true;
    return lower == "mono.dll" || lower == "monobdwgc-2.0.dll";
}

std::optional<RuntimeInfo> FindRuntime(core::IMemorySource& memory) {
    std::vector<core::ModuleInfo> ordered;
    for (const auto& module : memory.Modules())
        if (LooksLikeMonoModuleName(module.name)) ordered.push_back(module);
    for (const auto& module : memory.Modules())
        if (!LooksLikeMonoModuleName(module.name)) ordered.push_back(module);

    RuntimeInfo best;

    for (const auto& module : ordered) {
        const auto exports = core::ReadModuleExports(memory, module.base);
        if (exports.empty()) continue;

        int mono_count = 0;
        std::unordered_map<std::string, std::uint32_t> by_name;
        by_name.reserve(exports.size());
        for (const auto& e : exports) {
            by_name.emplace(e.name, e.rva);
            if (e.name.rfind("mono_", 0) == 0) ++mono_count;
        }
        if (mono_count == 0) continue;

        RuntimeInfo candidate;
        candidate.module_base  = module.base;
        candidate.module_size  = module.size;
        candidate.module_name  = module.name;
        candidate.mono_exports = mono_count;

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
            "{} exports {} mono_* entry points; {} of {} the walk needs resolved",
            module.name, mono_count, candidate.api.resolved, RequiredEntryPointCount()));

        if (candidate.api.Complete()) {
            candidate.confidence = 0.98f;
            candidate.evidence.emplace_back(
                "every entry point resolved by name, so no runtime version is involved");
        } else {
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
        if (best.api.Complete() && best.api.degraded.empty()) break;
    }

    if (best.api.resolved == 0) return std::nullopt;

    core::LogInfo("mono runtime: {} at {:#x}, {} exports, {}/{} entry points, {}/{} optional",
                  best.module_name, Raw(best.module_base), best.mono_exports,
                  best.api.resolved, RequiredEntryPointCount(),
                  best.api.enrichment, OptionalEntryPointCount());
    return best;
}

std::filesystem::path ManagedFolderBeside(const std::filesystem::path& exe) {
    std::error_code ec;
    const auto folder = exe.parent_path();
    if (!std::filesystem::is_directory(folder, ec)) return {};

    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (!entry.is_directory()) continue;
        const auto candidate = entry.path() / "Managed";
        if (std::filesystem::exists(candidate / "Assembly-CSharp.dll", ec)) return candidate;
    }
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (!entry.is_directory()) continue;
        const auto candidate = entry.path() / "Managed";
        if (std::filesystem::exists(candidate / "mscorlib.dll", ec)) return candidate;
    }
    return {};
}

std::vector<MonoProcess> DetectMonoProcesses() {
    std::vector<MonoProcess> found;
    std::error_code ec;

    const auto is_helper = [](std::string_view name) {
        return name == "UnityCrashHandler64.exe" || name == "UnityCrashHandler32.exe";
    };

    for (auto& process : core::EnumerateProcesses()) {
        if (process.path.empty()) continue;
        if (is_helper(process.name)) continue;

        const auto folder = std::filesystem::path(process.path).parent_path();
        if (std::filesystem::exists(folder / "GameAssembly.dll", ec)) continue;
        if (!std::filesystem::exists(folder / "UnityPlayer.dll", ec)) continue;
        if (ManagedFolderBeside(process.path).empty()) continue;

        MonoProcess entry;
        entry.runtime_loaded = RuntimeModuleLoaded(process.pid);
        entry.process        = std::move(process);
        found.push_back(std::move(entry));
    }
    return found;
}

} // namespace zircon::mono
