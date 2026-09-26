#pragma once

#include "core/MemorySource.h"
#include "core/PeImage.h"
#include "core/ProcessList.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace zircon::mono {

struct Api {
    core::Address get_root_domain{};
    core::Address domain_assembly_foreach{};
    core::Address assembly_get_image{};
    core::Address image_get_name{};
    core::Address image_get_table_info{};
    core::Address image_get_table_rows{};
    core::Address class_get{};

    core::Address class_get_name{};
    core::Address class_get_namespace{};
    core::Address class_get_parent{};
    core::Address class_get_flags{};
    core::Address class_get_type{};
    core::Address class_get_fields{};
    core::Address class_get_methods{};
    core::Address class_get_properties{};
    core::Address class_get_interfaces{};
    core::Address class_get_nested_types{};
    core::Address class_is_valuetype{};
    core::Address class_is_enum{};
    core::Address class_instance_size{};
    core::Address class_init{};

    core::Address field_get_name{};
    core::Address field_get_type{};
    core::Address field_get_offset{};
    core::Address field_get_flags{};

    core::Address method_get_name{};
    core::Address method_get_flags{};
    core::Address method_signature{};
    core::Address signature_get_return_type{};
    core::Address signature_get_params{};
    core::Address signature_get_param_count{};

    core::Address type_get_name{};
    core::Address type_get_type{};
    core::Address type_get_class{};

    core::Address thread_attach{};

    core::Address class_get_nesting_type{};
    core::Address class_get_image{};
    core::Address class_get_type_token{};
    core::Address class_get_element_class{};
    core::Address class_value_size{};
    core::Address class_min_align{};
    core::Address class_num_fields{};
    core::Address class_num_methods{};
    core::Address class_num_properties{};
    core::Address class_get_events{};
    core::Address class_num_events{};
    core::Address class_is_delegate{};
    core::Address class_is_blittable{};
    core::Address class_from_mono_type{};

    core::Address field_get_parent{};

    core::Address method_get_token{};
    core::Address method_get_class{};
    core::Address method_get_header{};

    core::Address property_get_name{};
    core::Address property_get_get_method{};
    core::Address property_get_set_method{};
    core::Address property_get_flags{};

    core::Address event_get_name{};

    core::Address type_get_name_full{};
    core::Address type_is_byref{};

    core::Address assembly_get_name{};
    core::Address assembly_name_get_name{};
    core::Address assembly_name_get_version{};
    core::Address image_get_filename{};
    core::Address image_get_assembly{};

    core::Address unity_generic_argument_count{};
    core::Address unity_generic_argument_at{};

    core::Address thread_detach{};
    core::Address free{};

    bool Complete() const { return missing.empty(); }

    std::vector<std::string> missing;
    std::vector<std::string> degraded;
    int resolved{0};
    int enrichment{0};
};

int RequiredEntryPointCount();
int OptionalEntryPointCount();

struct RuntimeInfo {
    core::Address module_base{};
    std::uint64_t module_size{};
    std::string   module_name;

    int mono_exports{0};

    Api api;

    float confidence{0.0f};
    std::vector<std::string> evidence;

    bool Valid() const { return !core::IsNull(module_base) && api.Complete(); }
};

bool LooksLikeMonoModuleName(std::string_view name);

std::optional<RuntimeInfo> FindRuntime(core::IMemorySource& memory);

struct MonoProcess {
    core::ProcessInfo process;
    bool runtime_loaded{false};
    std::string module_name;
};

std::vector<MonoProcess> DetectMonoProcesses();

std::filesystem::path ManagedFolderBeside(const std::filesystem::path& exe);

} // namespace zircon::mono
