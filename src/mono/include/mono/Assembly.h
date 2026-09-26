#pragma once

#include "core/Types.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace zircon::mono {

enum class Table : std::uint8_t {
    Module           = 0x00,
    TypeRef          = 0x01,
    TypeDef          = 0x02,
    Field            = 0x04,
    MethodDef        = 0x06,
    Param            = 0x08,
    InterfaceImpl    = 0x09,
    MemberRef        = 0x0A,
    Constant         = 0x0B,
    CustomAttribute  = 0x0C,
    FieldMarshal     = 0x0D,
    DeclSecurity     = 0x0E,
    ClassLayout      = 0x0F,
    FieldLayout      = 0x10,
    StandAloneSig    = 0x11,
    EventMap         = 0x12,
    Event            = 0x14,
    PropertyMap      = 0x15,
    Property         = 0x17,
    MethodSemantics  = 0x18,
    MethodImpl       = 0x19,
    ModuleRef        = 0x1A,
    TypeSpec         = 0x1B,
    ImplMap          = 0x1C,
    FieldRva         = 0x1D,
    Assembly         = 0x20,
    AssemblyRef      = 0x23,
    File             = 0x26,
    ExportedType     = 0x27,
    ManifestResource = 0x28,
    NestedClass      = 0x29,
    GenericParam     = 0x2A,
    MethodSpec       = 0x2B,
    GenericParamConstraint = 0x2C,
    Count            = 0x40,
};

struct FieldRow {
    std::uint16_t flags{0};
    std::string   name;
    std::uint32_t signature{0};
};

struct MethodRow {
    std::uint32_t rva{0};
    std::uint16_t impl_flags{0};
    std::uint16_t flags{0};
    std::string   name;
    std::uint32_t signature{0};
    std::uint32_t first_param{0};
    std::uint32_t param_count{0};
    std::uint32_t token{0};
};

struct PropertyRow {
    std::uint16_t flags{0};
    std::string   name;
    std::uint32_t signature{0};
    std::string   getter;
    std::string   setter;
};

struct TypeRow {
    std::uint32_t flags{0};
    std::string   name;
    std::string   name_space;
    std::uint32_t extends{0};
    std::uint32_t token{0};
    std::int32_t  enclosing{-1};

    std::vector<FieldRow>    fields;
    std::vector<MethodRow>   methods;
    std::vector<PropertyRow> properties;
    std::vector<std::string> interfaces;

    bool is_enum{false};
    bool is_interface{false};
    bool is_valuetype{false};
    bool is_abstract{false};
    std::string underlying;

    std::vector<std::pair<std::string, std::int64_t>> enum_values;
    bool enum_values_resolved{true};
};

struct AssemblyMetadata {
    std::string name;
    std::string version;
    std::string mvid;
    std::string file;
    std::string runtime_version;

    std::vector<TypeRow> types;

    std::string FullNameOf(std::size_t index) const;
};

core::Result<AssemblyMetadata> ReadAssembly(const std::string& path);

struct AssemblySetStats {
    std::size_t files{0};
    std::size_t read{0};
    std::size_t refused{0};
    std::size_t types{0};
    std::size_t enum_values{0};
    std::vector<std::string> refusals;
};

std::vector<AssemblyMetadata> ReadManagedFolder(const std::string& folder,
                                                AssemblySetStats& stats);

} // namespace zircon::mono
