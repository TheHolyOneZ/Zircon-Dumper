// The IR, described to itself.
//
// Every ZnKind gets a table of fields: name, type tag, and a captureless lambda that reads
// it off the C++ object. Those tables are the whole read side of the plugin ABI. zn->str,
// zn->len, zn->at and the introspection calls are all table lookups.
//
// Written out by hand, not generated, because they *are* the ABI. A field appearing here
// is a promise, and renaming one breaks every plugin that asked for it. Explicit tables
// keep that promise visible in a diff.

#include "plugin/Nodes.h"

#include <algorithm>

namespace zircon::plugin {
namespace {

using namespace zircon::ir;

ZnNode Wrap(const void* object, ZnKind kind) {
    ZnNode node{};
    node.obj  = object;
    node.kind = static_cast<std::uint32_t>(object ? kind : ZN_KIND_NONE);
    return node;
}

// The tables are repetitive by nature: ~90 fields, each a name, a tag and a one-line read.
// Longhand that's 400 lines with the interesting part, the field name and what it reads,
// buried in boilerplate. These four macros exist for that and live only in this file.
#define ZN_STR(NAME, T, EXPR)                                                              \
    Field{NAME, ZN_FIELD_STR, [](const void* p) -> std::string_view {                      \
        const auto& v = *static_cast<const T*>(p); (void)v; return EXPR; }}

#define ZN_NUM(NAME, T, EXPR)                                                              \
    Field{NAME, ZN_FIELD_I64, nullptr, [](const void* p) -> std::int64_t {                 \
        const auto& v = *static_cast<const T*>(p); (void)v;                                \
        return static_cast<std::int64_t>(EXPR); }}

#define ZN_F64(NAME, T, EXPR)                                                              \
    Field{NAME, ZN_FIELD_F64, nullptr, nullptr, [](const void* p) -> double {              \
        const auto& v = *static_cast<const T*>(p); (void)v;                                \
        return static_cast<double>(EXPR); }}

#define ZN_BOOL(NAME, T, EXPR)                                                             \
    Field{NAME, ZN_FIELD_BOOL, nullptr, nullptr, nullptr, [](const void* p) -> bool {      \
        const auto& v = *static_cast<const T*>(p); (void)v; return (EXPR); }}

#define ZN_NODE(NAME, T, KIND, EXPR)                                                       \
    Field{NAME, ZN_FIELD_NODE, nullptr, nullptr, nullptr, nullptr,                         \
        [](const void* p) -> ZnNode {                                                      \
            const auto& v = *static_cast<const T*>(p); (void)v;                            \
            return Wrap(&(EXPR), KIND); }}

// A list of nodes needs two reads, so the length and the element share one entry.
#define ZN_LIST(NAME, T, KIND, VEC)                                                        \
    Field{NAME, ZN_FIELD_LIST_NODE, nullptr, nullptr, nullptr, nullptr, nullptr,           \
        [](const void* p) -> std::size_t {                                                 \
            return static_cast<const T*>(p)->VEC.size(); },                                \
        [](const void* p, std::size_t i) -> ZnNode {                                       \
            return Wrap(&static_cast<const T*>(p)->VEC[i], KIND); }}

#define ZN_STRS(NAME, T, VEC)                                                              \
    Field{NAME, ZN_FIELD_LIST_STR, nullptr, nullptr, nullptr, nullptr, nullptr,            \
        [](const void* p) -> std::size_t {                                                 \
            return static_cast<const T*>(p)->VEC.size(); }, nullptr,                       \
        [](const void* p, std::size_t i) -> std::string_view {                             \
            return static_cast<const T*>(p)->VEC[i]; }}

// --- the tables ---------------------------------------------------------------------

const Field kDump[] = {
    ZN_NUM ("schema_version", Dump, v.schema_version),
    ZN_NODE("header",         Dump, ZN_KIND_HEADER, v.header),
    ZN_STRS("names",          Dump, names),
    ZN_LIST("packages",       Dump, ZN_KIND_PACKAGE, packages),
};

const Field kHeader[] = {
    ZN_STR ("tool_version", Header, v.tool_version),
    ZN_STR ("created_utc",  Header, v.created_utc),
    ZN_BOOL("partial",      Header, v.partial),
    ZN_NODE("source",       Header, ZN_KIND_SOURCE, v.source),
    ZN_NODE("engine",       Header, ZN_KIND_ENGINE, v.engine),
    ZN_LIST("offsets",      Header, ZN_KIND_OFFSET, offsets),
    ZN_STRS("globals",      Header, globals),
};

const Field kSource[] = {
    ZN_STR("kind",        SourceInfo, v.kind),
    ZN_STR("process",     SourceInfo, v.process),
    ZN_STR("main_module", SourceInfo, v.main_module),
    ZN_NUM("module_base", SourceInfo, v.module_base),
    ZN_NUM("image_size",  SourceInfo, v.image_size),
};

const Field kEngine[] = {
    ZN_STR ("version",              EngineInfo, v.version),
    ZN_F64 ("confidence",           EngineInfo, v.confidence),
    ZN_BOOL("uses_fproperty",       EngineInfo, v.uses_fproperty),
    ZN_BOOL("chunked_gobjects",     EngineInfo, v.chunked_gobjects),
    ZN_BOOL("chunked_name_pool",    EngineInfo, v.chunked_name_pool),
    ZN_BOOL("case_preserving_name", EngineInfo, v.case_preserving_name),
    ZN_STRS("evidence",             EngineInfo, evidence),
};

const Field kOffset[] = {
    ZN_STR("name",  DerivedOffset, v.name),
    ZN_NUM("value", DerivedOffset, v.value),
};

const Field kPackage[] = {
    ZN_STR ("name",    Package, v.name),
    ZN_LIST("classes", Package, ZN_KIND_STRUCT, classes),
    ZN_LIST("structs", Package, ZN_KIND_STRUCT, structs),
    ZN_LIST("enums",   Package, ZN_KIND_ENUM,   enums),
};

const Field kStruct[] = {
    ZN_STR ("name",           Struct, v.name),
    ZN_STR ("path",           Struct, v.path),
    ZN_STR ("super",          Struct, v.super),
    ZN_BOOL("is_class",       Struct, v.is_class),
    ZN_NUM ("size",           Struct, v.size),
    ZN_NUM ("alignment",      Struct, v.alignment),
    ZN_NUM ("inherited_size", Struct, v.inherited_size),
    ZN_NUM ("cpp_prefix",     Struct, v.cpp_prefix),
    ZN_NUM ("vtable_rva",     Struct, v.vtable_rva),
    ZN_STRS("interfaces",     Struct, interfaces),
    ZN_LIST("properties",     Struct, ZN_KIND_PROPERTY, properties),
    ZN_LIST("functions",      Struct, ZN_KIND_FUNCTION, functions),
};

const Field kEnum[] = {
    ZN_STR ("name",       Enum, v.name),
    ZN_STR ("path",       Enum, v.path),
    ZN_STR ("underlying", Enum, v.underlying),
    ZN_BOOL("is_flags",   Enum, v.is_flags),
    ZN_LIST("values",     Enum, ZN_KIND_ENUM_VALUE, values),
};

const Field kEnumValue[] = {
    ZN_STR("name",  EnumValue, v.name),
    ZN_NUM("value", EnumValue, v.value),
};

const Field kProperty[] = {
    ZN_STR ("name",        Property, v.name),
    ZN_NODE("type",        Property, ZN_KIND_TYPE, v.type),
    ZN_NUM ("offset",      Property, v.offset),
    ZN_NUM ("size",        Property, v.size),
    ZN_NUM ("array_dim",   Property, v.array_dim),
    ZN_NUM ("flags",       Property, v.flags),
    ZN_STRS("flag_names",  Property, flag_names),
    ZN_BOOL("is_bitfield", Property, v.is_bitfield),
    ZN_NUM ("byte_mask",   Property, v.byte_mask),
    ZN_NUM ("field_mask",  Property, v.field_mask),
    ZN_NUM ("bit_index",   Property, v.bit_index),
    ZN_STR ("default",     Property, v.default_value),
};

const Field kFunction[] = {
    ZN_STR ("name",            Function, v.name),
    ZN_NUM ("flags",           Function, v.flags),
    ZN_STRS("flag_names",      Function, flag_names),
    ZN_LIST("params",          Function, ZN_KIND_PARAM, params),
    ZN_NUM ("native_rva",      Function, v.native_rva),
    ZN_NUM ("script_size",     Function, v.script_size),
    ZN_LIST("script",          Function, ZN_KIND_STATEMENT, script),
    ZN_BOOL("script_complete", Function, v.script_complete),
};

const Field kParam[] = {
    ZN_STR ("name",      FunctionParam, v.name),
    ZN_NODE("type",      FunctionParam, ZN_KIND_TYPE, v.type),
    ZN_NUM ("offset",    FunctionParam, v.offset),
    ZN_NUM ("size",      FunctionParam, v.size),
    ZN_BOOL("is_return", FunctionParam, v.is_return),
    ZN_BOOL("is_out",    FunctionParam, v.is_out),
    ZN_BOOL("is_const",  FunctionParam, v.is_const),
};

const Field kStatement[] = {
    ZN_NUM("offset", ScriptStatement, v.offset),
    ZN_NUM("depth",  ScriptStatement, v.depth),
    ZN_STR("text",   ScriptStatement, v.text),
};

// `kind` goes out as a name, not a number. A script that has to know TypeKind::Array == 24
// is coupled to an enumerator order we're free to change. One comparing against "Array"
// isn't.
const Field kType[] = {
    ZN_STR ("kind",   TypeRef, ToString(v.kind)),
    ZN_STR ("name",   TypeRef, v.name),
    ZN_STR ("raw",    TypeRef, v.raw),
    ZN_NUM ("size",   TypeRef, v.size),
    ZN_LIST("params", TypeRef, ZN_KIND_TYPE, params),
};

const Field kOptions[] = {
    ZN_STR ("out_dir",        Options, v.out_dir),
    ZN_STR ("package_filter", Options, v.package_filter),
    ZN_BOOL("single_file",    Options, v.single_file),
    ZN_BOOL("allow_partial",  Options, v.allow_partial),
};

#undef ZN_STR
#undef ZN_NUM
#undef ZN_F64
#undef ZN_BOOL
#undef ZN_NODE
#undef ZN_LIST
#undef ZN_STRS

struct Table {
    const char*  kind_name;
    const Field* fields;
    std::size_t  count;
};

template <std::size_t N>
constexpr Table Make(const char* name, const Field (&fields)[N]) {
    return Table{name, fields, N};
}

// Indexed by ZnKind, so lookup is an array index.
const Table kTables[] = {
    Table{"none", nullptr, 0},
    Make("dump",       kDump),
    Make("header",     kHeader),
    Make("source",     kSource),
    Make("engine",     kEngine),
    Make("offset",     kOffset),
    Make("package",    kPackage),
    Make("struct",     kStruct),
    Make("enum",       kEnum),
    Make("enum_value", kEnumValue),
    Make("property",   kProperty),
    Make("function",   kFunction),
    Make("param",      kParam),
    Make("statement",  kStatement),
    Make("type",       kType),
    Make("options",    kOptions),
};

static_assert(std::size(kTables) == ZN_KIND_OPTIONS + 1,
              "every ZnKind needs a table, or kind_name indexes out of range");

const Table* TableFor(std::uint32_t kind) {
    if (kind == 0 || kind >= std::size(kTables)) return nullptr;
    return &kTables[kind];
}

} // namespace

const Field* FindField(std::uint32_t kind, const char* name) {
    const Table* table = TableFor(kind);
    if (!table || !name) return nullptr;

    const std::string_view want{name};
    for (std::size_t i = 0; i < table->count; ++i)
        if (table->fields[i].name == want) return &table->fields[i];
    return nullptr;
}

const char* KindName(std::uint32_t kind) {
    const Table* table = TableFor(kind);
    return table ? table->kind_name : "none";
}

std::size_t FieldCount(std::uint32_t kind) {
    const Table* table = TableFor(kind);
    return table ? table->count : 0;
}

const char* FieldNameAt(std::uint32_t kind, std::size_t index) {
    const Table* table = TableFor(kind);
    if (!table || index >= table->count) return nullptr;
    return table->fields[index].name.data();
}

ZnNode MakeNode(const void* object, ZnKind kind) { return Wrap(object, kind); }

} // namespace zircon::plugin
