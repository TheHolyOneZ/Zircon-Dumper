// Tests for the plugin ABI.
//
// The one that matters is TestRoundTrip. It builds a Dump with every field set to a
// distinctive value, reads the whole thing back through nothing but ZnApi calls,
// reconstructs a Dump from what came out, and asserts the two are equal.
//
// That is a real completeness check. If a field is added to the
// IR and its entry is forgotten in Nodes.cpp, the reconstruction cannot read it: the call
// returns ZN_ERR_NO_FIELD and the rebuilt value stays default, so both the status
// assertion and the equality assertion fail. A table of getters is exactly the kind of
// thing that falls behind the type it describes unnoticed, and this is what stops it.

#include "plugin/Api.h"
#include "plugin/Nodes.h"

#include "ir/Model.h"
#include "zircon/plugin.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace zircon;
using namespace zircon::ir;

namespace {

int g_failures = 0;
int g_checks   = 0;

void Check(bool condition, const char* expression, const char* file, int line) {
    ++g_checks;
    if (condition) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expression);
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

const ZnApi* zn = nullptr;

// --- a fixture in which no field holds its default ------------------------------------
//
// Defaults are the enemy of a round-trip test: a field that is never read still compares
// equal if both sides left it zero. So everything here is set to something distinctive.

TypeRef MakeType(TypeKind kind, std::string name, std::string raw, std::int32_t size) {
    TypeRef type;
    type.kind = kind;
    type.name = std::move(name);
    type.raw  = std::move(raw);
    type.size = size;
    return type;
}

Dump BuildFixture() {
    Dump dump;
    dump.schema_version = kSchemaVersion;

    dump.header.tool_version = "0.1.0-test";
    dump.header.created_utc  = "2026-09-13T00:00:00Z";
    dump.header.partial      = true;
    dump.header.source       = {"external", "Game.exe", "Game.exe", 0x140000000ull, 0x4000000ull};

    dump.header.engine.version              = "5.6";
    dump.header.engine.confidence           = 0.75f;
    dump.header.engine.uses_fproperty       = true;
    dump.header.engine.chunked_gobjects     = true;
    dump.header.engine.chunked_name_pool    = false;
    dump.header.engine.case_preserving_name = true;
    dump.header.engine.evidence             = {"first reason", "second reason"};

    dump.header.offsets = {{"UObject.InternalIndex", 0x0C}, {"UStruct.SuperStruct", 0x40}};
    dump.header.globals = {"GObjects=0x7c70d0", "FNamePool=0x6e3590"};

    dump.names = {"None", "Actor", "StaticMesh"};

    Property simple;
    simple.name        = "Health";
    simple.type        = MakeType(TypeKind::Float, "", "FloatProperty", 4);
    simple.offset      = 0x40;
    simple.size        = 8;
    simple.array_dim   = 2;
    simple.flags       = 0x0000000000000081ull;
    simple.flag_names  = {"Edit", "Parm"};
    simple.is_bitfield = false;
    simple.default_value = "2.5";

    Property packed;
    packed.name        = "bHidden";
    packed.type        = MakeType(TypeKind::Bool, "", "BoolProperty", 1);
    packed.offset      = 0x48;
    packed.size        = 1;
    packed.array_dim   = 1;
    packed.flags       = 0x0000000000000002ull;
    packed.flag_names  = {"ConstParm"};
    packed.is_bitfield = true;
    packed.byte_mask   = 0xFF;
    packed.field_mask  = 0x04;
    packed.bit_index   = 2;
    packed.default_value = "true";

    // A container, so the nested TypeRef list gets exercised and not only leaf types.
    Property container;
    container.name       = "Tags";
    container.type       = MakeType(TypeKind::Array, "", "ArrayProperty", 16);
    container.type.params.push_back(MakeType(TypeKind::Name, "", "NameProperty", 8));
    container.offset     = 0x50;
    container.size       = 16;
    container.array_dim  = 1;
    container.flags      = 0x0000000000000004ull;
    container.flag_names = {"OutParm"};
    container.default_value = "[]";

    FunctionParam in_param;
    in_param.name   = "Amount";
    in_param.type   = MakeType(TypeKind::Int32, "", "IntProperty", 4);
    in_param.offset = 0;
    in_param.size   = 4;
    in_param.is_out = true;
    in_param.is_const = true;

    FunctionParam ret;
    ret.name      = "ReturnValue";
    ret.type      = MakeType(TypeKind::Bool, "", "BoolProperty", 1);
    ret.offset    = 4;
    ret.size      = 1;
    ret.is_return = true;

    Function function;
    function.name            = "ApplyDamage";
    function.flags           = 0x00020401u;
    function.flag_names      = {"Final", "Native", "Public"};
    function.params          = {in_param, ret};
    function.native_rva      = 0x123456ull;
    function.script_size     = 42;
    function.script          = {{0u, 0, "Health = Health - Amount;"},
                                {12u, 1, "return true;"}};
    function.script_complete = false;

    Struct klass;
    klass.name           = "Actor";
    klass.path           = "/Script/Engine.Actor";
    klass.super          = "/Script/CoreUObject.Object";
    klass.is_class       = true;
    klass.size           = 0x290;
    klass.alignment      = 8;
    klass.inherited_size = 0x28;
    klass.cpp_prefix     = 'A';
    klass.vtable_rva     = 0x4C1230ull;
    klass.interfaces     = {"/Script/Engine.Interface_AssetUserData"};
    klass.properties     = {simple, packed, container};
    klass.functions      = {function};

    Struct record;
    record.name           = "Vector";
    record.path           = "/Script/CoreUObject.Vector";
    record.super          = "";
    record.is_class       = false;
    record.cpp_prefix     = 'F';
    record.size           = 24;
    record.alignment      = 8;
    record.inherited_size = 0;
    record.properties     = {simple};

    Enum enumeration;
    enumeration.name       = "EMovementMode";
    enumeration.path       = "/Script/Engine.EMovementMode";
    enumeration.underlying = "uint8";
    enumeration.is_flags   = true;
    enumeration.values     = {{"MOVE_None", 0}, {"MOVE_Walking", 1}, {"MOVE_Max", 255}};

    Package package;
    package.name    = "/Script/Engine";
    package.classes = {klass};
    package.structs = {record};
    package.enums   = {enumeration};

    dump.packages = {package};
    return dump;
}

// --- reading it back through nothing but the ABI ---------------------------------------

std::string Str(ZnNode node, const char* field) {
    const char* text = nullptr;
    std::size_t len  = 0;
    CHECK(zn->str(node, field, &text, &len) == ZN_OK);
    return text ? std::string(text, len) : std::string{};
}

std::int64_t I64(ZnNode node, const char* field) {
    std::int64_t value = 0;
    CHECK(zn->i64(node, field, &value) == ZN_OK);
    return value;
}

bool Bool(ZnNode node, const char* field) {
    int value = 0;
    CHECK(zn->bln(node, field, &value) == ZN_OK);
    return value != 0;
}

std::size_t Len(ZnNode node, const char* field) {
    std::size_t count = 0;
    CHECK(zn->len(node, field, &count) == ZN_OK);
    return count;
}

ZnNode At(ZnNode node, const char* field, std::size_t index) {
    ZnNode element{};
    CHECK(zn->at(node, field, index, &element) == ZN_OK);
    return element;
}

std::vector<std::string> Strings(ZnNode node, const char* field) {
    std::vector<std::string> out;
    const std::size_t count = Len(node, field);
    for (std::size_t i = 0; i < count; ++i) {
        const char* text = nullptr;
        std::size_t len  = 0;
        CHECK(zn->at_str(node, field, i, &text, &len) == ZN_OK);
        out.emplace_back(text ? std::string(text, len) : std::string{});
    }
    return out;
}

ZnNode Sub(ZnNode node, const char* field) {
    ZnNode child{};
    CHECK(zn->sub(node, field, &child) == ZN_OK);
    return child;
}

TypeRef ReadType(ZnNode node) {
    TypeRef type;
    const auto kind = TypeKindFromString(Str(node, "kind"));
    CHECK(kind.has_value());
    type.kind = kind.value_or(TypeKind::Unknown);
    type.name = Str(node, "name");
    type.raw  = Str(node, "raw");
    type.size = static_cast<std::int32_t>(I64(node, "size"));

    const std::size_t count = Len(node, "params");
    for (std::size_t i = 0; i < count; ++i)
        type.params.push_back(ReadType(At(node, "params", i)));
    return type;
}

Property ReadProperty(ZnNode node) {
    Property p;
    p.name        = Str(node, "name");
    p.type        = ReadType(Sub(node, "type"));
    p.offset      = static_cast<std::int32_t>(I64(node, "offset"));
    p.size        = static_cast<std::int32_t>(I64(node, "size"));
    p.array_dim   = static_cast<std::int32_t>(I64(node, "array_dim"));
    p.flags       = static_cast<std::uint64_t>(I64(node, "flags"));
    p.flag_names  = Strings(node, "flag_names");
    p.is_bitfield = Bool(node, "is_bitfield");
    p.byte_mask   = static_cast<std::uint8_t>(I64(node, "byte_mask"));
    p.field_mask  = static_cast<std::uint8_t>(I64(node, "field_mask"));
    p.bit_index   = static_cast<std::int32_t>(I64(node, "bit_index"));
    p.default_value = Str(node, "default");
    return p;
}

Function ReadFunction(ZnNode node) {
    Function f;
    f.name       = Str(node, "name");
    f.flags      = static_cast<std::uint32_t>(I64(node, "flags"));
    f.flag_names = Strings(node, "flag_names");

    const std::size_t params = Len(node, "params");
    for (std::size_t i = 0; i < params; ++i) {
        ZnNode raw = At(node, "params", i);
        FunctionParam p;
        p.name      = Str(raw, "name");
        p.type      = ReadType(Sub(raw, "type"));
        p.offset    = static_cast<std::int32_t>(I64(raw, "offset"));
        p.size      = static_cast<std::int32_t>(I64(raw, "size"));
        p.is_return = Bool(raw, "is_return");
        p.is_out    = Bool(raw, "is_out");
        p.is_const  = Bool(raw, "is_const");
        f.params.push_back(std::move(p));
    }

    f.native_rva   = static_cast<std::uint64_t>(I64(node, "native_rva"));
    f.script_size  = static_cast<std::int32_t>(I64(node, "script_size"));

    const std::size_t statements = Len(node, "script");
    for (std::size_t i = 0; i < statements; ++i) {
        ZnNode raw = At(node, "script", i);
        ScriptStatement statement;
        statement.offset = static_cast<std::uint32_t>(I64(raw, "offset"));
        statement.depth  = static_cast<std::int32_t>(I64(raw, "depth"));
        statement.text   = Str(raw, "text");
        f.script.push_back(std::move(statement));
    }

    f.script_complete = Bool(node, "script_complete");
    return f;
}

Struct ReadStruct(ZnNode node) {
    Struct record;
    record.name           = Str(node, "name");
    record.path           = Str(node, "path");
    record.super          = Str(node, "super");
    record.is_class       = Bool(node, "is_class");
    record.size           = static_cast<std::int32_t>(I64(node, "size"));
    record.alignment      = static_cast<std::int32_t>(I64(node, "alignment"));
    record.inherited_size = static_cast<std::int32_t>(I64(node, "inherited_size"));
    record.cpp_prefix     = static_cast<char>(I64(node, "cpp_prefix"));
    record.vtable_rva     = static_cast<std::uint64_t>(I64(node, "vtable_rva"));
    record.interfaces     = Strings(node, "interfaces");

    const std::size_t properties = Len(node, "properties");
    for (std::size_t i = 0; i < properties; ++i)
        record.properties.push_back(ReadProperty(At(node, "properties", i)));

    const std::size_t functions = Len(node, "functions");
    for (std::size_t i = 0; i < functions; ++i)
        record.functions.push_back(ReadFunction(At(node, "functions", i)));

    return record;
}

Dump ReadDump(ZnNode node) {
    Dump dump;
    dump.schema_version = static_cast<int>(I64(node, "schema_version"));
    dump.names          = Strings(node, "names");

    ZnNode header = Sub(node, "header");
    dump.header.tool_version = Str(header, "tool_version");
    dump.header.created_utc  = Str(header, "created_utc");
    dump.header.partial      = Bool(header, "partial");
    dump.header.globals      = Strings(header, "globals");

    ZnNode source = Sub(header, "source");
    dump.header.source.kind        = Str(source, "kind");
    dump.header.source.process     = Str(source, "process");
    dump.header.source.main_module = Str(source, "main_module");
    dump.header.source.module_base = static_cast<std::uint64_t>(I64(source, "module_base"));
    dump.header.source.image_size  = static_cast<std::uint64_t>(I64(source, "image_size"));

    ZnNode engine = Sub(header, "engine");
    dump.header.engine.version = Str(engine, "version");
    double confidence = 0.0;
    CHECK(zn->f64(engine, "confidence", &confidence) == ZN_OK);
    dump.header.engine.confidence           = static_cast<float>(confidence);
    dump.header.engine.uses_fproperty       = Bool(engine, "uses_fproperty");
    dump.header.engine.chunked_gobjects     = Bool(engine, "chunked_gobjects");
    dump.header.engine.chunked_name_pool    = Bool(engine, "chunked_name_pool");
    dump.header.engine.case_preserving_name = Bool(engine, "case_preserving_name");
    dump.header.engine.evidence             = Strings(engine, "evidence");

    const std::size_t offsets = Len(header, "offsets");
    for (std::size_t i = 0; i < offsets; ++i) {
        ZnNode raw = At(header, "offsets", i);
        dump.header.offsets.push_back(
            {Str(raw, "name"), static_cast<std::int32_t>(I64(raw, "value"))});
    }

    const std::size_t packages = Len(node, "packages");
    for (std::size_t i = 0; i < packages; ++i) {
        ZnNode raw = At(node, "packages", i);
        Package package;
        package.name = Str(raw, "name");

        const std::size_t classes = Len(raw, "classes");
        for (std::size_t j = 0; j < classes; ++j)
            package.classes.push_back(ReadStruct(At(raw, "classes", j)));

        const std::size_t structs = Len(raw, "structs");
        for (std::size_t j = 0; j < structs; ++j)
            package.structs.push_back(ReadStruct(At(raw, "structs", j)));

        const std::size_t enums = Len(raw, "enums");
        for (std::size_t j = 0; j < enums; ++j) {
            ZnNode source_enum = At(raw, "enums", j);
            Enum enumeration;
            enumeration.name       = Str(source_enum, "name");
            enumeration.path       = Str(source_enum, "path");
            enumeration.underlying = Str(source_enum, "underlying");
            enumeration.is_flags   = Bool(source_enum, "is_flags");

            const std::size_t values = Len(source_enum, "values");
            for (std::size_t k = 0; k < values; ++k) {
                ZnNode value = At(source_enum, "values", k);
                enumeration.values.push_back({Str(value, "name"), I64(value, "value")});
            }
            package.enums.push_back(std::move(enumeration));
        }
        dump.packages.push_back(std::move(package));
    }
    return dump;
}

void TestRoundTrip() {
    const Dump original = BuildFixture();
    const ZnNode node = plugin::MakeNode(&original, ZN_KIND_DUMP);

    const Dump rebuilt = ReadDump(node);
    CHECK(rebuilt == original);

    // Narrow the report when it does fail: equality alone would not say where.
    CHECK(rebuilt.header == original.header);
    CHECK(rebuilt.names == original.names);
    CHECK(rebuilt.packages == original.packages);
}

void TestErrors() {
    const Dump dump = BuildFixture();
    const ZnNode node = plugin::MakeNode(&dump, ZN_KIND_DUMP);

    const char* text = nullptr;
    std::int64_t number = 0;
    std::size_t count = 0;
    ZnNode child{};

    // A field this kind does not have, and a field of the wrong type, are different
    // mistakes and must report differently: one is a typo, the other a misunderstanding.
    CHECK(zn->str(node, "no_such_field", &text, nullptr) == ZN_ERR_NO_FIELD);
    CHECK(zn->i64(node, "header", &number) == ZN_ERR_WRONG_TYPE);
    CHECK(zn->str(node, "packages", &text, nullptr) == ZN_ERR_WRONG_TYPE);

    // Past the end of a list.
    CHECK(zn->at(node, "packages", 99, &child) == ZN_ERR_RANGE);
    CHECK(zn->at_str(node, "names", 99, &text, nullptr) == ZN_ERR_RANGE);

    // A null node and a null out-parameter are both the caller's bug, not a crash.
    ZnNode empty{};
    CHECK(zn->str(empty, "tool_version", &text, nullptr) == ZN_ERR_NULL);
    CHECK(zn->i64(node, "schema_version", nullptr) == ZN_ERR_NULL);
    CHECK(zn->len(node, nullptr, &count) == ZN_ERR_NULL);

    // Introspection over a null node must be answerable, not undefined.
    CHECK(zn->field_count(empty) == 0);
    CHECK(zn->field_type(empty, "anything") == 0);
    CHECK(std::string(zn->kind_name(empty)) == "none");
}

void TestIntrospection() {
    const Dump dump = BuildFixture();
    const ZnNode node = plugin::MakeNode(&dump, ZN_KIND_DUMP);

    CHECK(std::string(zn->kind_name(node)) == "dump");
    CHECK(zn->field_count(node) == 4);

    // Every name field_name reports must be readable by that name, or `pairs(node)` in a
    // script would produce keys that then fail to index.
    const std::size_t fields = zn->field_count(node);
    for (std::size_t i = 0; i < fields; ++i) {
        const char* name = zn->field_name(node, i);
        CHECK(name != nullptr);
        if (name) CHECK(zn->field_type(node, name) != 0);
    }

    CHECK(zn->field_name(node, 99) == nullptr);

    ZnNode package = At(node, "packages", 0);
    CHECK(std::string(zn->kind_name(package)) == "package");
    CHECK(zn->field_type(package, "classes") == ZN_FIELD_LIST_NODE);
    CHECK(zn->field_type(package, "name") == ZN_FIELD_STR);
}

// A plugin is third-party code. It names a file; the host decides where that lands, and
// the answer is never outside the directory the user pointed at.
void TestWriteSandbox() {
    namespace fs = std::filesystem;

    // A unique leaf, so two runs of the suite in parallel do not fight over the same
    // directory and report each other's files.
    const fs::path root = fs::temp_directory_path() / "zircon_plugin_test" /
                          std::to_string(static_cast<unsigned long>(::GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    plugin::EmitContext context;
    context.options.out_dir = root.string();

    const std::string body = "hello";

    // The ordinary case, including a subdirectory that does not exist yet.
    CHECK(context.Write("plain.txt", body) == ZN_OK);
    CHECK(context.Write("nested/deeper/file.txt", body) == ZN_OK);
    CHECK(fs::exists(root / "plain.txt"));
    CHECK(fs::exists(root / "nested" / "deeper" / "file.txt"));
    CHECK(context.result.files.size() == 2);

    // Every way out of the directory.
    CHECK(context.Write("../escaped.txt", body)        == ZN_ERR_BAD_PATH);
    CHECK(context.Write("nested/../../escaped.txt", body) == ZN_ERR_BAD_PATH);
    CHECK(context.Write("..", body)                    == ZN_ERR_BAD_PATH);
    CHECK(context.Write("C:/Windows/escaped.txt", body) == ZN_ERR_BAD_PATH);
    CHECK(context.Write("/etc/passwd", body)           == ZN_ERR_BAD_PATH);
    CHECK(context.Write("", body)                      == ZN_ERR_BAD_PATH);

    // Nothing refused may have been written, and nothing refused may be counted.
    CHECK(context.result.files.size() == 2);
    CHECK(!fs::exists(root.parent_path() / "escaped.txt"));

    fs::remove_all(root, ec);
}

void TestVersioning() {
    CHECK(zn->size == sizeof(ZnApi));
    CHECK(zn->abi_major == ZN_ABI_MAJOR);
    CHECK(zn->abi_minor == ZN_ABI_MINOR);
    CHECK(zn->schema_version == ZN_SCHEMA_VERSION);

    // The ABI advertises a schema; the IR defines one. They have to agree, or a plugin
    // that checks the advertised number is checking a number that means nothing.
    CHECK(ZN_SCHEMA_VERSION == kSchemaVersion);

    // No entry in the vtable may be null: a plugin calls through it without checking.
    const void* const* slots = reinterpret_cast<const void* const*>(&zn->str);
    const std::size_t count =
        (sizeof(ZnApi) - offsetof(ZnApi, str)) / sizeof(void*);
    for (std::size_t i = 0; i < count; ++i) CHECK(slots[i] != nullptr);
}

} // namespace

int main() {
    zn = plugin::HostApi();

    TestRoundTrip();
    TestErrors();
    TestIntrospection();
    TestWriteSandbox();
    TestVersioning();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
